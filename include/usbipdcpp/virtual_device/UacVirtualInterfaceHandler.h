#pragma once

#include <atomic>
#include <deque>
#include <memory>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

class UacAudioStreamingSourceHandler;
class UacAudioStreamingSinkHandler;

/// UAC device configuration - Control AudioControl interface descriptor and Feature Unit behavior
struct UacDeviceConfig {
    /// Input terminal type. Defaults to microphone (ITT_MICROPHONE);
    /// TT_USB_STREAMING in the speaker direction (data flows in from USB), at this time the output terminal uses output_terminal_type
    std::uint16_t input_terminal_type = ITT_MICROPHONE;
    /// Output terminal type (only used in the speaker direction, default Speaker)
    std::uint16_t output_terminal_type = ITT_SPEAKER;
    /// number of channels (1 or 2). 0 means inferred from AudioSource
    std::uint8_t channels = 0;
    /// Feature Unit Whether to provide mute control
    bool feature_unit_mute = true;
    /// Feature Unit Whether to provide volume control
    bool feature_unit_volume = true;
    /// Volume range lower limit (1/256 dB unit, default -42dB)
    std::int16_t volume_min_db256 = -0x2A00;
    /// Volume range upper limit (1/256 dB unit, default 0dB)
    std::int16_t volume_max_db256 = 0;
};

/// AudioControl interface handler - class descriptor + Feature Unit (mute/volume) control
class USBIPDCPP_API UacAudioControlHandler : public VirtualInterfaceHandler {
public:
    UacAudioControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /// Apply device configuration (must finish before UacDeviceHelper::setup_microphone calls setup_interface_handlers)
    void set_config(const UacDeviceConfig &config);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;
    void on_setup_interface_handlers() override;

    /// Current mute status (for AS handler query)
    [[nodiscard]] bool is_muted() const {
        return mute;
    }

    /// Current volume linear gain, Q16 fixed point (0dB = 65536, -∞ = 0), for AS handler scaling samples
    [[nodiscard]] std::uint32_t volume_scale_q16() const {
        return gain_q16;
    }

    /**
     * @brief Establish association with AS handler (UacDeviceHelper is called during assembly, choose one of the microphone/speaker directions)
     *
     * AC descriptor Header's baInterfaceNr refers to the AS interface number, obtained during assembly from the AS
     * handler's get_interface(): associations between interfaces are explicitly established during assembly, without
     * guessing interface numbers while building descriptors (consistent with UVC's vc/vs mutual-pointer pattern; in composite devices
     * AC does not start from interface 0, hard coding 1 will cause the driver to not find the AS interface)
     * @param handler AS interface handler (UacAudioStreamingSourceHandler or
     *                UacAudioStreamingSinkHandler)
     */
    void set_as_handler(UacAudioStreamingSourceHandler *handler);
    void set_as_handler(UacAudioStreamingSinkHandler *handler);

private:
    void build_class_descriptor();

    /// Push UAC1 status word via AC interrupt endpoint (aligned audio_notify of kernel f_uac1.c:
    /// Cache when there is no pending URB, the interrupt IN URB submitted subsequently by the host is obtained immediately, and the status is not lost)
    void send_ac_status(data_type status);

    data_type class_desc;
    bool desc_built = false;
    UacDeviceConfig config{};
    bool mute = false;
    std::int16_t volume_db = 0; // unit 1/256 dB, 0 represents 0dB
    std::uint32_t gain_q16 = 65536; // Q16 representation of 10^(volume_db/256/20)
    // AC Header’s baInterfaceNr (AS interface number) is written during set_as_handler assembly.
    // Default 1 maintains the old behavior of single device form (AC=0 → AS=1)
    std::uint8_t as_interface_number = 1;

    // AC interrupt endpoint: temporary storage when state changes with no pending URB (2-byte UAC1 status word).
    // push non-blocking: respond directly if there is a pending request, otherwise enter the buffer (aligned kernel f_uac1 audio_notify)
    MessageInChannel status_channel;
};

/// AudioStreaming interface handler — class descriptor + sample rate negotiation + ISO PCM push stream
class USBIPDCPP_API UacAudioStreamingSourceHandler : public VirtualInterfaceHandler {
public:
    UacAudioStreamingSourceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<AudioSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    // Linux snd-usb-audio sends the sampling rate control of UAC1 to the endpoint (recipient=Endpoint, wIndex=endpoint address),
    // rather than interface (implementation difference allowed by UAC 1.0 specification), needs to be processed at the endpoint level entry
    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer, std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    // The class descriptor (General + Format Type) of  AS must appear in each alt,
    // Otherwise, the host driver is in alt 1. The format descriptor cannot be found and cannot be parsed.
    [[nodiscard]] bool put_class_specific_descriptor_in_all_alts() const override {
        return true;
    }

    AudioSource *get_source() {
        return source.get();
    }

    void set_ac_handler(UacAudioControlHandler *handler) {
        ac_handler = handler;
    }

private:
    /// Collect all sample rates supported by the source under the current number of channels/bit depth (deduplication in ascending order)
    [[nodiscard]] std::vector<std::uint32_t> supported_sample_rates(const AudioFormatInfo &fmt) const;
    void build_class_descriptor();

    /// Handle sampling rate control request (GET_CUR/GET_MIN/GET_MAX/SET_CUR).
    /// The interface level and endpoint level entries are shared (Linux snd-usb-audio is initiated from the endpoint level).
    /// Returning true means that the response has been submitted by itself, and the caller returns directly; false means that it is not recognized.
    bool handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request, GenericTransfer *trx,
                                      TransferHandle &transfer, std::uint32_t transfer_buffer_length);

    /// Fill dst with n bytes of PCM: pull data from source, pad with silence if insufficient; apply mute/volume
    void fill_pcm(std::uint8_t *dst, std::size_t n);

    /// Update per-microframe packet-byte scheduling parameters from the current sample rate (consistent with kernel gadget u_audio.c)
    void update_packet_bytes();

    /// TransferScheduler processor (notification semantics): called by the scheduling thread at service time (after the data duration
    /// elapses), fill PCM data, send the response independently through session.submit_ret_submit,
    /// and report completion through on_urb_done (same-endpoint serialization depends on the report)
    void process_iso_in(TransferResponder &session, const UsbEndpoint &ep, std::uint32_t seqnum, int num_iso_packets,
                        TransferHandle transfer);

    UacAudioControlHandler *ac_handler = nullptr;

    std::unique_ptr<AudioSource> source;
    data_type class_desc;
    bool streaming = false;

    // ISO IN packet scheduling parameters, consistent with kernel gadget u_audio.c (u_audio_start_playback):
    // Bytes per packet = base packet length + one frame when accumulated remainder overflows; packet length is always an integer multiple of frame size
    // (without splitting interleaved samples), with average rate exactly matching the sample rate; sample rates not divisible by 8kHz
    // (such as 44100) also work correctly. Recalculated by update_packet_bytes when sample rate changes
    std::size_t packet_framesize = 0;   // Bytes per PCM frame = channel count × bytes per sample
    std::size_t packet_interval = 0;    // Packets per second: high-speed bInterval=1 → one packet per microframe
    std::size_t packet_base = 0;        // Base packet length = frame size × (sample rate/interval)
    std::size_t packet_residue_step = 0; // Remainder accumulated per packet = frame size × (sample rate%interval)
    std::size_t packet_residue_acc = 0;  // Remainder accumulator (continuous across packets/URBs)

    // Current audio chunk reference + consumed offset (continuous stream across URBs)
    const std::uint8_t *chunk_data = nullptr;
    std::size_t chunk_size = 0;
    std::size_t chunk_offset = 0;
};

/// AudioStreaming interface handler (speaker direction): class descriptors + sample-rate negotiation + ISO OUT receive stream
/// Symmetric with UacAudioStreamingSourceHandler (microphone IN push stream): reverse the data plane to consume host-sent
/// PCM (write to AudioSink); the control plane (sample-rate negotiation, class descriptors) has identical logic
class USBIPDCPP_API UacAudioStreamingSinkHandler : public VirtualInterfaceHandler {
public:
    UacAudioStreamingSinkHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                 std::unique_ptr<AudioSink> sink);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    // Linux snd-usb-audio sends the sampling rate control of UAC1 to the endpoint (recipient=Endpoint, wIndex=endpoint address),
    // rather than interface (implementation difference allowed by UAC 1.0 specification), needs to be processed at the endpoint level entry
    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer, std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    // The class descriptor (General + Format Type) of  AS must appear in each alt,
    // Otherwise, the host driver is in alt 1. The format descriptor cannot be found and cannot be parsed.
    [[nodiscard]] bool put_class_specific_descriptor_in_all_alts() const override {
        return true;
    }

    AudioSink *get_sink() {
        return sink.get();
    }

    void set_ac_handler(UacAudioControlHandler *handler) {
        ac_handler = handler;
    }

private:
    void build_class_descriptor();

    /// Handle sampling rate control request (GET_CUR/GET_MIN/GET_MAX/SET_CUR).
    /// The interface level and endpoint level entries are shared (Linux snd-usb-audio is initiated from the endpoint level).
    /// Returning true means that the response has been submitted by itself, and the caller returns directly; false means that it is not recognized.
    bool handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request, GenericTransfer *trx,
                                      TransferHandle &transfer, std::uint32_t transfer_buffer_length);

    /// TransferScheduler processor (notification semantics): called by the scheduling thread at service time (after the data duration
    /// afterwards), consuming PCM (apply volume + write to sink + receive-stream feedback statistics), independently using
    /// session.submit_ret_submit to send the response and on_urb_done to report completion
    /// (same-endpoint serialization depends on the report)
    void process_iso_out(TransferResponder &session, const UsbEndpoint &ep, std::uint32_t seqnum, int num_iso_packets,
                         TransferHandle transfer);

    UacAudioControlHandler *ac_handler = nullptr;

    std::unique_ptr<AudioSink> sink;
    data_type class_desc;
    bool streaming = false;

    // Receive-rate feedback loop: each host URB submission has fixed overhead (usbip-win receives RET → resubmits →
    // TCP round trip, measured ~60µs); delaying responses by the data duration makes the receive rate consistently lower than the consumption rate,
    // so the buffer level gradually falls, causing periodic underruns (playback noise). Use receive rate measured over wall-clock windows as feedback,
    // adjusting response delay (negative = early response) to lock the receive rate to the consumption rate. Units: µs.
    // Atomic: read by the network thread (calculating data_duration when handle submits), written by the scheduling thread
    //(process_iso_out feedback statistics)
    std::atomic<std::int64_t> pacing_delta_us{0};
    // Feedback rate measurement: every 100 URBs (about 1 second), calculate receive rate as the increase in cumulative received / wall-clock time
    std::uint32_t urb_stat_count = 0;
    std::uint64_t urb_stat_bytes = 0;
    std::int64_t urb_stat_last_bytes = 0;   // Cumulative received bytes at the previous window
    std::int64_t urb_stat_last_time_us = 0; // Wall-clock time of the previous window (steady_clock µs)
};

/// UAC device helper: assemble UAC functionality on device (AC + AS interface handlers, associated with each other)
class USBIPDCPP_API UacDeviceHelper {
public:
    /**
     * @brief Assemble UAC microphone functionality on device (two adjacent AC + AS interfaces)
     * @param device Target device (interfaces must already be in device->interfaces, with stable addresses)
     * @param ac_interface_number interface_number of the first interface (AC) of the UAC function;
     *        find AS by interface_number = ac_interface_number + 1 in device->interfaces
     *        (function interface numbers are consecutive, independent of array indices). Handlers of other interfaces on the device
     *        are unaffected
     * @param string_pool String pool
     * @param source Audio source
     * @param config UAC device configuration (channel count, etc.)
     * @return Error code on failure (such as missing AC/AS interfaces), default-constructed on success (no error)
     */
    static std::error_code setup_microphone(std::shared_ptr<UsbDevice> device, std::uint8_t ac_interface_number,
                                            StringPool &string_pool, std::unique_ptr<AudioSource> source,
                                            const UacDeviceConfig &config = {});

    /**
     * @brief Assemble UAC speaker functionality on device (two adjacent AC + AS interfaces, ISO OUT receive stream)
     * @param device Target device (interfaces must already be in device->interfaces, with stable addresses)
     * @param ac_interface_number interface_number of the first interface (AC) of the UAC function;
     *        find AS by interface_number = ac_interface_number + 1. Handlers of other interfaces
     *        on the device are unaffected
     * @param string_pool String pool
     * @param sink Audio sink
     * @param config UAC device configuration (channel count, etc.); AC input_terminal_type is forced to
     *        TT_USB_STREAMING (speaker topology)
     * @return Error code on failure (such as missing AC/AS interfaces), default-constructed on success (no error)
     */
    static std::error_code setup_speaker(std::shared_ptr<UsbDevice> device, std::uint8_t ac_interface_number,
                                         StringPool &string_pool, std::unique_ptr<AudioSink> sink,
                                         const UacDeviceConfig &config = {});
};

} // namespace usbipdcpp
