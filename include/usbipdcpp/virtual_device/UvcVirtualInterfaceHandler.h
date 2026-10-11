#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <system_error>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// PROBE/COMMIT negotiation struct (UVC 1.5, 48 bytes)
struct UvcStreamingControl {
    std::uint16_t bmHint = 0;
    std::uint8_t bFormatIndex = 1;
    std::uint8_t bFrameIndex = 1;
    std::uint32_t dwFrameInterval = 333333;
    std::uint16_t wKeyFrameRate = 0;
    std::uint16_t wPFrameRate = 0;
    std::uint16_t wCompQuality = 0;
    std::uint16_t wCompWindowSize = 0;
    std::uint16_t wDelay = 0;
    std::uint32_t dwMaxVideoFrameSize = 0;
    std::uint32_t dwMaxPayloadTransferSize = 0;
    std::uint32_t dwClockFrequency = 27000000;
    std::uint8_t bmFramingInfo = 0x03;
    std::uint8_t bPreferredVersion = 1;
    std::uint8_t bMinVersion = 1;
    std::uint8_t bMaxVersion = 1;
    // UVC 1.5 additional fields
    std::uint8_t bUsage = 0;
    std::uint8_t bBitDepthLuma = 0;
    std::uint8_t bmSettings = 0;
    std::uint8_t bMaxNumberOfRefFramesPlus1 = 0;
    std::uint16_t bmRateControlModes = 0;
    std::uint64_t bmLayoutPerStream = 0;

    static constexpr std::size_t SIZE = 48; // UVC 1.5 full size

    data_type serialize() const;
    void deserialize(const std::uint8_t *data, std::size_t len);
};

class UvcVideoStreamingHandler;

/// VideoControl interface handler: PU property queries/settings + coordination with the VS interface
class USBIPDCPP_API UvcVideoControlHandler : public VirtualInterfaceHandler {
public:
    explicit UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

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
    void on_setup_interface_handlers() override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    void set_vs_handler(UvcVideoStreamingHandler *handler) {
        vs_handler_ = handler;
    }

private:
    void build_class_descriptor();
    void send_vc_status(data_type status);

    data_type class_desc_;
    bool desc_built_ = false;
    bool power_on_ = true;
    UvcVideoStreamingHandler *vs_handler_ = nullptr;

    /**
     * @brief Status notification channel (message mode, interrupt IN endpoint)
     *
     * Encapsulates "suspend-response": suspend host interrupt IN requests first, then match responses when send_vc_status() pushes
     * status events (UVC 1.5 status notification, 6 bytes)
     */
    MessageInChannel status_channel;
};

/// VideoStreaming interface handler: PROBE/COMMIT + ISO stream push
class USBIPDCPP_API UvcVideoStreamingHandler : public VirtualInterfaceHandler {
public:
    UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<VideoSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    VideoSource *get_source() {
        return source_.get();
    }

    void set_vc_handler(UvcVideoControlHandler *handler) {
        vc_handler_ = handler;
    }

    /// VC handler requests stream stop (Video Power Mode off, etc.)
    void notify_stop_streaming() {
        streaming_ = false;
    }

private:
    void build_class_descriptor();

    UvcVideoControlHandler *vc_handler_ = nullptr;

    std::unique_ptr<VideoSource> source_;
    data_type class_desc_;
    UvcStreamingControl probe_data_{};
    bool committed_ = false;
    bool streaming_ = false;

    // Frame buffer
    std::vector<std::uint8_t> frame_buffer_;
    std::size_t frame_offset_ = 0;
    bool current_fid_ = false;

    // Frame clock: current frame start time + negotiated frame interval. Matches physical camera semantics: the frame clock determines
    // frame rate, which does not drift with the host's consumption speed:
    // - Insufficient bandwidth (cannot transfer a frame within one frame interval): drop frames and switch to the latest frame; the image stays current, but frame rate is lower
    // - Excess bandwidth (frame transferred before the frame interval ends): send empty packets to await the next frame, avoiding accelerated playback
    // Before the first frame, use epoch (now is always ≥ it + interval), ensuring the first frame starts immediately when streaming begins
    std::chrono::steady_clock::time_point frame_started_at_{};
    std::chrono::microseconds frame_interval_{};
};

/// UVC device helper: assemble UVC functionality on device (VC + VS interface handlers, mutual associations, IAD)
class USBIPDCPP_API UvcDeviceHelper {
public:
    /**
     * @brief Assemble UVC functionality on device (two adjacent VC + VS interfaces)
     * @param device Target device (interfaces must already be in device->interfaces, with stable addresses)
     * @param vc_interface_number interface_number of the UVC function's first interface (VC);
     *        find VS by interface_number = vc_interface_number + 1 in
     *        device->interfaces (IAD requires consecutive function interface numbers, independent of array
     *        indices: in composite devices UVC interfaces may not be at the beginning of interfaces).
     *        Other interface handlers on the device are unaffected (only bind the two VC/VS interfaces)
     * @param string_pool String pool
     * @param source Video source
     * @return Error code on failure (such as missing VC/VS interfaces), default-constructed on success (no error)
     */
    static std::error_code setup(std::shared_ptr<UsbDevice> device, std::uint8_t vc_interface_number,
                                 StringPool &string_pool, std::unique_ptr<VideoSource> source);
};

} // namespace usbipdcpp
