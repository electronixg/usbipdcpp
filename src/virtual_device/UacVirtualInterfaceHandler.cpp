// [UAC-AC]/[UAC-AS]/[ISO] debug logs are removed at compile time by default (TRACE level):
// define USBIPDCPP_STRACE to retain them when investigating host-driver negotiation/stream issues
#ifndef USBIPDCPP_STRACE
    #undef SPDLOG_ACTIVE_LEVEL
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#else
    #undef SPDLOG_ACTIVE_LEVEL
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "spdlog/spdlog.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {

// ==================== UacAudioControlHandler ====================

UacAudioControlHandler::UacAudioControlHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    change_string_interface(L"Usbipdcpp Audio Control");
}

void UacAudioControlHandler::set_config(const UacDeviceConfig &new_config) {
    config = new_config;
    // Only 1 or 2 channels supported (bControlSize=1)
    if (config.channels != 1 && config.channels != 2) {
        config.channels = 1;
    }
}

void UacAudioControlHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UacAudioControlHandler::set_as_handler(UacAudioStreamingSourceHandler *handler) {
    as_interface_number = static_cast<std::uint8_t>(handler->get_interface().interface_number);
}

void UacAudioControlHandler::set_as_handler(UacAudioStreamingSinkHandler *handler) {
    as_interface_number = static_cast<std::uint8_t>(handler->get_interface().interface_number);
}

void UacAudioControlHandler::build_class_descriptor() {
    if (desc_built)
        return;
    desc_built = true;

    // UAC 1.0:Header(9) + Input Terminal(12) + Feature Unit(7+ch+1) + Output Terminal(9)
    // Microphone topology: IT(MIC) -> FU -> OT(USB streaming); speaker topology: IT(USB streaming) -> FU
    // -> OT(Speaker) (matching kernel gadget f_uac1.c: usb_out_it_desc/io_out_ot_desc).
    // Determine direction from input_terminal_type: data flowing in from USB (IT=TT_USB_STREAMING) means speaker
    bool is_speaker = (config.input_terminal_type == TT_USB_STREAMING);
    // Header baInterfaceNr is the audio-streaming (AS) interface number: explicitly written by set_as_handler
    // during assembly (AC does not start at interface 0 in composite devices; hardcoding prevents drivers finding the AS interface)
    auto as_if_num = as_interface_number;
    auto fu_len = static_cast<std::uint8_t>(AC_FEATURE_UNIT_FIXED_LEN + config.channels + 1);

    // Feature Unit bmaControls: combine according to configuration
    std::uint8_t bma_controls = 0;
    if (config.feature_unit_mute)
        bma_controls |= 0x01;
    if (config.feature_unit_volume)
        bma_controls |= 0x02;

    data_type d;

    // AC Header (placeholder wTotalLength; fill actual total length after construction)
    AcHeaderDesc{AC_HEADER_LEN, CS_INTERFACE, AC_DESC_HEADER,
                 UAC_BCD_1_00, 0x00,
                 0x01, as_if_num}
            .append_to(d);

    // Input Terminal: microphone-direction type comes from configuration (default microphone); speaker direction is USB streaming
    auto channel_config = (config.channels == 2) ? CHANNEL_CONFIG_STEREO : CHANNEL_CONFIG_MONO;
    AcInputTerminalDesc{AC_INPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_INPUT_TERMINAL,
                        UAC_ENTITY_INPUT_TERMINAL, config.input_terminal_type,
                        0x00, config.channels, channel_config,
                        0x00, 0x00}
            .append_to(d);

    // Feature Unit: bmaControls array has ch+1 elements (master + each logical channel); all channels share the same control set
    AcFeatureUnitHead{fu_len, CS_INTERFACE, AC_DESC_FEATURE_UNIT,
                      UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL,
                      0x01}
            .append_to(d, bma_controls, config.channels);

    // Output Terminal: microphone direction is USB streaming; speaker direction is output_terminal_type
    auto output_type = is_speaker ? config.output_terminal_type : TT_USB_STREAMING;
    AcOutputTerminalDesc{AC_OUTPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_OUTPUT_TERMINAL,
                         UAC_ENTITY_OUTPUT_TERMINAL, output_type,
                         0x00, UAC_ENTITY_FEATURE_UNIT,
                         0x00}
            .append_to(d);

    // Fill AC Header wTotalLength (offset 5-6) with the actual descriptor total length
    d[5] = static_cast<std::uint8_t>(d.size() & 0xFF);
    d[6] = static_cast<std::uint8_t>((d.size() >> 8) & 0xFF);

    class_desc = std::move(d);
}

data_type UacAudioControlHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioControlHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Debug: print received AC control requests (investigating host-driver stream startup/volume initialization)
    SPDLOG_TRACE("[UAC-AC] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // Handle class-specific GET_DESCRIPTOR (CS_INTERFACE=0x24)
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // UAC 1.0 section 5.2.3: wValue high byte is the control selector, low byte is channel number (master=0), wIndex high byte is entity
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    // Input/Output Terminals have no control attributes; ACK empty data to prevent host STALL (as UVC does for IT/OT)
    if (entity == UAC_ENTITY_INPUT_TERMINAL || entity == UAC_ENTITY_OUTPUT_TERMINAL) {
        trx->actual_length = 0;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, std::move(transfer)));
        return;
    }

    if (entity != UAC_ENTITY_FEATURE_UNIT) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // ===== Feature Unit =====
    switch (control_selector) {
        case FU_MUTE_CONTROL: {
            if (!config.feature_unit_mute) {
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }
            switch (request) {
                case GET_CUR:
                    trx->data = {static_cast<std::uint8_t>(mute ? 1 : 0)};
                    trx->actual_length = 1;
                    break;
                case SET_CUR:
                    if (!trx->data.empty()) {
                        auto new_mute = (trx->data[0] != 0);
                        if (new_mute != mute) {
                            mute = new_mute;
                            // Push state changes through the AC interrupt endpoint (matching kernel u_audio_mute_put notify)
                            send_ac_status({UAC1_STATUS_TYPE_IRQ_PENDING | UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF,
                                            UAC_ENTITY_FEATURE_UNIT});
                        }
                    }
                    trx->actual_length = 0;
                    break;
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        case FU_VOLUME_CONTROL: {
            if (!config.feature_unit_volume) {
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }
            switch (request) {
                case GET_CUR: {
                    auto v = volume_db;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_MIN: {
                    auto v = config.volume_min_db256;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_MAX: {
                    auto v = config.volume_max_db256;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_DEF: {
                    std::int16_t v = 0;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_RES: {
                    // 1dB step
                    std::int16_t v = 0x0100;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case SET_CUR: {
                    if (trx->data.size() >= 2) {
                        auto v = static_cast<std::int16_t>(trx->data[0] | (trx->data[1] << 8));
                        // Clamp to the configured volume range
                        auto clamped = std::clamp(v, config.volume_min_db256, config.volume_max_db256);
                        if (clamped != volume_db) {
                            volume_db = clamped;
                            // Q16 linear gain: 10^(dB/20); below -120dB counts as muted
                            double linear = std::pow(10.0, (volume_db / 256.0) / 20.0);
                            if (volume_db <= -0x7800)
                                linear = 0.0;
                            gain_q16 = static_cast<std::uint32_t>(linear * 65536.0 + 0.5);
                            // Push state changes through the AC interrupt endpoint (matching kernel u_audio_volume_put notify)
                            send_ac_status({UAC1_STATUS_TYPE_IRQ_PENDING | UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF,
                                            UAC_ENTITY_FEATURE_UNIT});
                        }
                    }
                    trx->actual_length = 0;
                    break;
                }
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        default:
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
    }

    // Debug: print submitted responses (first 2 data bytes + length) to investigate host-driver initialization failures
    SPDLOG_TRACE("[UAC-AC] Submit response seq={} actual={} d0=0x{:02x} d1=0x{:02x}", seqnum, trx->actual_length,
                trx->data.empty() ? 0 : trx->data[0], trx->data.size() < 2 ? 0 : trx->data[1]);

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
}

void UacAudioControlHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       std::error_code &ec) {
    if (ep.is_in()) {
        // Handled within the channel: reply immediately if state is buffered; otherwise suspend waiting for send_ac_status()
        status_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioControlHandler::send_ac_status(data_type status) {
    // Handled within the channel: reply directly to pending requests; otherwise buffer (matching kernel f_uac1 audio_notify)
    status_channel.push(std::move(status));
}

void UacAudioControlHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    status_channel.on_new_connection(&current_session);
}

void UacAudioControlHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically), then let the parent clear session
    status_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioControlHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = status_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's LibusbDeviceHandler
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

data_type UacAudioControlHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacAudioStreamingSourceHandler ====================

UacAudioStreamingSourceHandler::UacAudioStreamingSourceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                   std::unique_ptr<AudioSource> source) :
    VirtualInterfaceHandler(handle_interface, string_pool), source(std::move(source)) {
    change_string_interface(L"Usbipdcpp Microphone");

    // Calculate packet scheduling parameters from the initial format (recalculated by SET_CUR when sample rate/channel count changes)
    update_packet_bytes();
}

void UacAudioStreamingSourceHandler::update_packet_bytes() {
    auto fmt = source->current_format();
    // High-speed isochronous endpoint bInterval=4 (matching kernel gadget f_uac1.c's as_in_ep_desc):
    // one packet per 1ms, 1000 packets per second.
    // Base packet length = frame size x (sample rate/1000); convert the sample rate remainder modulo 1000 into frames and accumulate per packet;
    // when the accumulator reaches a frame, add one frame to this packet. Packet lengths are always integral frame sizes, with average rate exactly equal to sample rate
    packet_framesize = static_cast<std::size_t>(fmt.channels) * (fmt.bits_per_sample / 8);
    packet_interval = 1000;
    auto rate = static_cast<std::size_t>(fmt.sample_rate);
    packet_base = packet_framesize * (rate / packet_interval);
    packet_residue_step = packet_framesize * (rate % packet_interval);
    packet_residue_acc = 0;
}

void UacAudioStreamingSourceHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

std::vector<std::uint32_t> UacAudioStreamingSourceHandler::supported_sample_rates(const AudioFormatInfo &fmt) const {
    // Collect all sample rates supported by source for the current channel count/bit depth; deduplicate and sort ascending
    std::vector<std::uint32_t> rates;
    for (auto &f: source->supported_formats()) {
        if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
            rates.push_back(f.sample_rate);
        }
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

void UacAudioStreamingSourceHandler::build_class_descriptor() {
    auto fmt = source->current_format();
    auto rates = supported_sample_rates(fmt);

    data_type d;
    // AS General: bTerminalLink points to AC's Output Terminal (USB streaming)
    AsGeneralDesc{AS_GENERAL_LEN, CS_INTERFACE, AS_DESC_GENERAL,
                  UAC_ENTITY_OUTPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM}
            .append_to(d);

    // Format Type I: 16-bit PCM; sample rate list from source (UAC 1.0 permits up to 255)
    AsFormatTypeIHead{static_cast<std::uint8_t>(AS_FORMAT_TYPE_I_BASE_LEN + rates.size() * AS_SAMFREQ_ENTRY_LEN),
                      CS_INTERFACE, AS_DESC_FORMAT_TYPE,
                      0x01, // bFormatType: FORMAT_TYPE_I
                      static_cast<std::uint8_t>(fmt.channels),
                      0x02, // bSubframeSize
                      static_cast<std::uint8_t>(fmt.bits_per_sample),
                      static_cast<std::uint8_t>(rates.size())}
            .append_to(d, rates);

    class_desc = std::move(d);
}

data_type UacAudioStreamingSourceHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioStreamingSourceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Debug: print received AS control requests (investigating host-driver stream startup)
    SPDLOG_TRACE("[UAC-AS] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // Handle class-specific GET_DESCRIPTOR (CS_INTERFACE=0x24)
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            SPDLOG_TRACE("[UAC-AS] Return {} bytes of CS_INTERFACE class descriptors", class_desc.size());
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // AS interface control entity is always the interface itself (0)
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    if (entity != 0 || control_selector != AS_SAMPLING_FREQ_CONTROL) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    if (!handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioStreamingSourceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Linux snd-usb-audio sends UAC1 sample-rate controls to the endpoint:
    // wValue high byte is the control selector, wIndex is endpoint address (recpient=Endpoint)
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    if (control_selector != AS_SAMPLING_FREQ_CONTROL ||
        !handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

bool UacAudioStreamingSourceHandler::handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request,
                                                            GenericTransfer *trx, TransferHandle &transfer,
                                                            std::uint32_t transfer_buffer_length) {
    auto fmt = source->current_format();
    switch (request) {
        case GET_CUR: {
            auto rate = fmt.sample_rate;
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case GET_MIN:
        case GET_MAX: {
            // Return minimum/maximum sample rates in the supported list
            auto rates = supported_sample_rates(fmt);
            auto rate = (request == GET_MIN) ? rates.front() : rates.back();
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case SET_CUR: {
            if (trx->data.size() < 3) {
                return false;
            }
            auto rate = static_cast<std::uint32_t>(trx->data[0] | (trx->data[1] << 8) | (trx->data[2] << 16));
            // UAC 1.0 section 5.2.3.2.3.1: discrete-sample-rate endpoints should round unsupported received values to the nearest supported value
            auto rates = supported_sample_rates(fmt);
            auto nearest = *std::min_element(rates.begin(), rates.end(), [rate](std::uint32_t a, std::uint32_t b) {
                return std::abs(static_cast<std::int64_t>(a) - rate) <
                       std::abs(static_cast<std::int64_t>(b) - rate);
            });
            if (nearest != rate) {
                SPDLOG_INFO("Sample rate SET_CUR {} not in supported list; rounding to {}", rate, nearest);
            }
            rate = nearest;
            if (!source->set_format(fmt.channels, fmt.bits_per_sample, rate)) {
                SPDLOG_WARN("Sample rate SET_CUR {} rejected by source", rate);
                return false;
            }
            update_packet_bytes();
            SPDLOG_INFO("Sample rate SET_CUR: {} -> source switched successfully, base packet length={} remainder step={}", rate,
                        packet_base, packet_residue_step);
            chunk_data = nullptr;
            chunk_size = 0;
            chunk_offset = 0;
            build_class_descriptor();
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
            return true;
        }
        default:
            return false;
    }

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
    return true;
}

void UacAudioStreamingSourceHandler::fill_pcm(std::uint8_t *dst, std::size_t n) {
    // Fill directly with 0 when muted
    if (ac_handler && ac_handler->is_muted()) {
        std::memset(dst, 0, n);
        return;
    }

    auto scale = ac_handler ? ac_handler->volume_scale_q16() : 65536;

    while (n > 0) {
        if (chunk_offset >= chunk_size) {
            AudioChunk chunk;
            if (!source->get_chunk(chunk)) {
                // No source data: fill the remainder with silence
                std::memset(dst, 0, n);
                return;
            }
            chunk_data = chunk.data;
            chunk_size = chunk.size;
            chunk_offset = 0;
        }
        auto take = std::min(n, chunk_size - chunk_offset);
        if (scale == 65536) {
            // 0dB fast path: copy directly
            std::memcpy(dst, chunk_data + chunk_offset, take);
        }
        else if (scale == 0) {
            std::memset(dst, 0, take);
        }
        else {
            // Scale each sample by Q16 (16-bit signed little-endian)
            const auto *src = reinterpret_cast<const std::int16_t *>(chunk_data + chunk_offset);
            auto *out = reinterpret_cast<std::int16_t *>(dst);
            for (std::size_t i = 0; i < take / 2; ++i) {
                out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(src[i]) * scale) >> 16);
            }
        }
        dst += take;
        chunk_offset += take;
        n -= take;
    }
}

void UacAudioStreamingSourceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                           std::uint32_t transfer_flags,
                                                           std::uint32_t transfer_buffer_length,
                                                           TransferHandle transfer, int num_iso_packets,
                                                           std::error_code &ec) {
    if (!streaming) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    // Response delay = audio duration corresponding to the sum of host-declared packet lengths (iso IN buffer is compactly filled according to each packet's
    // length; sum(length) is this transfer's data amount. The amount is known only during processing,
    // so convert the declared value in advance here; it differs from processing's total_sent only by the truncated remainder).
    // Move data filling to TransferScheduler's service time (vudc semantics: send RET immediately after processing;
    // limit the rate by delaying processing time. Each URB sends immediately after processing -> host resubmits immediately
    // -> next URB waits another data duration -> completion cadence always equals data rate, equivalent to the URB's N packets
    // completing across N frames. Immediate processing removes throttling from the host driver's completion->resubmission loop;
    // 158x excess sending was observed)
    std::chrono::microseconds data_duration{};
    auto fmt = source->current_format();
    auto bytes_per_second = static_cast<std::uint64_t>(fmt.sample_rate) * (fmt.bits_per_sample / 8) * fmt.channels;
    std::uint32_t declared = 0;
    for (auto &iso: trx->iso_descriptors)
        declared += iso.length;
    if (bytes_per_second > 0 && declared > 0) {
        data_duration = std::chrono::microseconds(
                static_cast<std::int64_t>(declared) * 1000000 / static_cast<std::int64_t>(bytes_per_second));
    }
    device_handler->get_transfer_scheduler().submit(
            *responder, ep, EndpointAttributes::Isochronous, data_duration, seqnum, std::move(transfer),
            [this, num_iso_packets](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&transfer) {
                process_iso_in(responder, ep, seqnum, num_iso_packets, std::move(transfer));
            });
}

void UacAudioStreamingSourceHandler::process_iso_in(TransferResponder &responder, const UsbEndpoint &ep,
                                                    std::uint32_t seqnum, int num_iso_packets,
                                                    TransferHandle transfer) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // Debug: confirm whether ISO URBs arrive and bytes filled per packet (investigating no-data recording on Windows)
    SPDLOG_TRACE("[ISO] seq={} num_packets={} packet_base={} residue_step={}",
                seqnum, num_iso_packets, packet_base, packet_residue_step);

    // Kernel usb_submit_urb initializes iso_frame_desc[n].status to -EXDEV;
    // clear it to zero, or the kernel audio driver skips every packet.
    for (auto &iso: iso_descs)
        iso.status = 0;

    // Calculate each packet's bytes using the remainder accumulation algorithm in kernel gadget u_audio.c (u_audio_iso_complete's PLAYBACK branch):
    // base packet length + one frame when the accumulated remainder reaches a frame.
    // Packet lengths are always integral frame sizes, with average rate exactly matching sample rate; for sample rates not integral multiples of 1kHz
    // (e.g. 44100), 88.2 bytes per ms is achieved by alternating 88/90-byte packets;
    // short packets are valid (CS_ENDPOINT bmAttributes D7=0; MaxPacketsOnly not set)
    std::uint32_t total_sent = 0;

    for (int i = 0; i < num_iso_packets; ++i) {
        auto &iso = iso_descs[i];
        packet_residue_acc += packet_residue_step;
        auto packet_bytes = packet_base;
        if (packet_residue_acc >= packet_framesize * packet_interval) {
            packet_bytes += packet_framesize;
            packet_residue_acc -= packet_framesize * packet_interval;
        }
        auto want = std::min(static_cast<std::size_t>(iso.length), packet_bytes);
        if (want == 0)
            continue;

        fill_pcm(&data[iso.offset], want);
        iso.actual_length = static_cast<std::uint32_t>(want);
        total_sent += iso.actual_length;
    }

    // Debug: ISO response information (distribution of actual bytes per packet)
    SPDLOG_TRACE("[ISO] Processing complete seq={} num_packets={} total={}B", seqnum, num_iso_packets, total_sent);
    // Send itself after processing (notification semantics, consistent with handler's other response paths), then report
    // completion: the scheduler advances the serialization point and serves the next URB (synchronous processing; send before reporting)
    responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_sent, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
    device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
}

void UacAudioStreamingSourceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
}

void UacAudioStreamingSourceHandler::on_disconnection(error_code &ec) {
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioStreamingSourceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        streaming = true;
        chunk_offset = 0; // Restart pulling data to ensure a clean stream start
        packet_residue_acc = 0; // Clear remainder (matching gadget uac_pcm_open resetting p_residue)
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

data_type UacAudioStreamingSourceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                            std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacAudioStreamingSinkHandler ====================

UacAudioStreamingSinkHandler::UacAudioStreamingSinkHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                           std::unique_ptr<AudioSink> sink) :
    VirtualInterfaceHandler(handle_interface, string_pool), sink(std::move(sink)) {
    change_string_interface(L"Usbipdcpp Speaker");
}

void UacAudioStreamingSinkHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UacAudioStreamingSinkHandler::build_class_descriptor() {
    auto fmt = sink->current_format();
    std::vector<std::uint32_t> rates;
    for (auto &f: sink->supported_formats()) {
        if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
            rates.push_back(f.sample_rate);
        }
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());

    data_type d;
    // AS General: bTerminalLink points to AC's Input Terminal (USB streaming; speaker-direction
    // data enters through this terminal, matching kernel gadget f_uac1.c as_out_header_desc's dynamic terminal link)
    AsGeneralDesc{AS_GENERAL_LEN, CS_INTERFACE, AS_DESC_GENERAL,
                  UAC_ENTITY_INPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM}
            .append_to(d);

    // Format Type I: 16-bit PCM; sample rate list from sink (UAC 1.0 permits up to 255)
    AsFormatTypeIHead{static_cast<std::uint8_t>(AS_FORMAT_TYPE_I_BASE_LEN + rates.size() * AS_SAMFREQ_ENTRY_LEN),
                      CS_INTERFACE, AS_DESC_FORMAT_TYPE,
                      0x01, // bFormatType: FORMAT_TYPE_I
                      static_cast<std::uint8_t>(fmt.channels),
                      0x02, // bSubframeSize
                      static_cast<std::uint8_t>(fmt.bits_per_sample),
                      static_cast<std::uint8_t>(rates.size())}
            .append_to(d, rates);

    class_desc = std::move(d);
}

data_type UacAudioStreamingSinkHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioStreamingSinkHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Debug: print received AS control requests (investigating host-driver stream startup)
    SPDLOG_TRACE("[UAC-AS-SINK] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // Handle class-specific GET_DESCRIPTOR (CS_INTERFACE=0x24)
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // AS interface control entity is always the interface itself (0)
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    if (entity != 0 || control_selector != AS_SAMPLING_FREQ_CONTROL) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    if (!handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioStreamingSinkHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Linux snd-usb-audio sends UAC1 sample-rate controls to the endpoint:
    // wValue high byte is the control selector, wIndex is endpoint address (recpient=Endpoint)
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    if (control_selector != AS_SAMPLING_FREQ_CONTROL ||
        !handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

bool UacAudioStreamingSinkHandler::handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request,
                                                                GenericTransfer *trx, TransferHandle &transfer,
                                                                std::uint32_t transfer_buffer_length) {
    auto fmt = sink->current_format();
    switch (request) {
        case GET_CUR: {
            auto rate = fmt.sample_rate;
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case GET_MIN:
        case GET_MAX: {
            // Return minimum/maximum sample rates in the supported list
            std::vector<std::uint32_t> rates;
            for (auto &f: sink->supported_formats()) {
                if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
                    rates.push_back(f.sample_rate);
                }
            }
            std::sort(rates.begin(), rates.end());
            auto rate = (request == GET_MIN) ? rates.front() : rates.back();
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case SET_CUR: {
            if (trx->data.size() < 3) {
                return false;
            }
            auto rate = static_cast<std::uint32_t>(trx->data[0] | (trx->data[1] << 8) | (trx->data[2] << 16));
            // UAC 1.0 section 5.2.3.2.3.1: discrete-sample-rate endpoints should round unsupported received values to the nearest supported value
            std::vector<std::uint32_t> rates;
            for (auto &f: sink->supported_formats()) {
                if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
                    rates.push_back(f.sample_rate);
                }
            }
            std::sort(rates.begin(), rates.end());
            auto nearest = *std::min_element(rates.begin(), rates.end(), [rate](std::uint32_t a, std::uint32_t b) {
                return std::abs(static_cast<std::int64_t>(a) - rate) <
                       std::abs(static_cast<std::int64_t>(b) - rate);
            });
            if (nearest != rate) {
                SPDLOG_INFO("Sample rate SET_CUR {} not in supported list; rounding to {}", rate, nearest);
            }
            rate = nearest;
            if (!sink->set_format(fmt.channels, fmt.bits_per_sample, rate)) {
                SPDLOG_WARN("Sample rate SET_CUR {} rejected by sink", rate);
                return false;
            }
            SPDLOG_INFO("Sample rate SET_CUR: {} -> sink switched successfully", rate);
            build_class_descriptor();
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
            return true;
        }
        default:
            return false;
    }

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
    return true;
}

void UacAudioStreamingSinkHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                               std::uint32_t transfer_flags,
                                                               std::uint32_t transfer_buffer_length,
                                                               TransferHandle transfer, int num_iso_packets,
                                                               std::error_code &ec) {
    if (!streaming) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    // Response delay = audio duration corresponding to the sum of host-declared packet lengths (OUT data is compactly arranged according to each packet's length;
    // sum(length) is this transfer's actual data amount, counted only during processing, so convert the declared value in
    // advance here) + closed-loop receive-rate correction. Delay follows data amount -> completion rate always equals host data rate
    //(matching real adaptive OUT endpoints' following behavior; fixed delays from a local clock accumulate frequency offsets from the host clock;
    // 0.2%-level loss of synchronization was observed -> usbaudio rate-matching buffer level gradually falls -> periodic
    // underrun noise). Move data processing (volume/writing sink/closed-loop statistics) to TransferScheduler's
    // service time (vudc semantics: send RET immediately after processing, limiting the rate by delaying processing time. Each
    // URB sends immediately after processing -> host resubmits immediately -> next URB waits another data duration -> completion cadence
    // always equals host data rate. Immediate responses cause excess sending; observed input at 5x speed exceeds consumption,
    // forcing 80% data loss -> accelerated/intermittent playback)
    std::chrono::microseconds data_duration{};
    auto fmt = sink->current_format();
    auto bytes_per_second = static_cast<std::uint64_t>(fmt.sample_rate) * (fmt.bits_per_sample / 8) * fmt.channels;
    std::uint32_t declared = 0;
    for (auto &iso: trx->iso_descriptors)
        declared += iso.length;
    if (bytes_per_second > 0 && declared > 0) {
        data_duration = std::chrono::microseconds(
                static_cast<std::int64_t>(declared) * 1000000 / static_cast<std::int64_t>(bytes_per_second));
        data_duration += std::chrono::microseconds(pacing_delta_us.load());
    }
    device_handler->get_transfer_scheduler().submit(
            *responder, ep, EndpointAttributes::Isochronous, data_duration, seqnum, std::move(transfer),
            [this, num_iso_packets](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&transfer) {
                process_iso_out(responder, ep, seqnum, num_iso_packets, std::move(transfer));
            });
}

void UacAudioStreamingSinkHandler::process_iso_out(TransferResponder &responder, const UsbEndpoint &ep,
                                                   std::uint32_t seqnum, int num_iso_packets,
                                                   TransferHandle transfer) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // iso descriptor status initially is -EXDEV (matching kernel usb_submit_urb initialization);
    // clear it to zero, or the kernel audio driver treats all packets as failed
    for (auto &iso: iso_descs)
        iso.status = 0;

    // OUT data is compactly arranged in the buffer by accumulating each packet's length (descriptor offset is the client's local layout,
    // untrusted; see LibusbTransferOperator recv_transfer_data comment); read each packet and pass it to
    // sink for consumption
    std::uint32_t total_received = 0;
    std::size_t data_pos = 0;
    for (auto &iso: iso_descs) {
        auto take = std::min(static_cast<std::size_t>(iso.length), data.size() - data_pos);
        if (take > 0) {
            // Apply AC volume control (mute/volume), matching microphone fill_pcm gain processing.
            // Windows volume sliders use FU SET_CUR; volume state is in the AC handler, so the receive side
            // must apply it for adjustments to work. Multiply each 16-bit PCM sample by Q16 gain; byte operations avoid misalignment
            if (ac_handler && ac_handler->is_muted()) {
                std::memset(&data[data_pos], 0, take);
            } else if (ac_handler) {
                auto scale = ac_handler->volume_scale_q16();
                if (scale != 65536) {
                    auto n = take / 2 * 2; // Process only complete samples
                    for (std::size_t i = 0; i < n; i += 2) {
                        auto s = static_cast<std::int32_t>(
                                static_cast<std::int16_t>(data[data_pos + i] | (data[data_pos + i + 1] << 8)));
                        s = (s * static_cast<std::int32_t>(scale)) >> 16;
                        data[data_pos + i] = static_cast<std::uint8_t>(s & 0xFF);
                        data[data_pos + i + 1] = static_cast<std::uint8_t>((s >> 8) & 0xFF);
                    }
                }
            }
            sink->write_pcm(&data[data_pos], take);
            iso.actual_length = static_cast<std::uint32_t>(take);
            total_received += iso.actual_length;
        }
        data_pos += iso.length;
    }
    // Closed-loop receive-rate control (unconditionally enabled for every sink):
    // Observed usbaudio data per URB is always 10ms of audio, never increasing/decreasing the amount; host submission cadence =
    // our response delay +/- small overhead -> receive rate is determined entirely by response delay. Feedback measures receive rate in a wall-clock window
    // (received cumulative increment/real time), not URB arrival intervals (usbip-win's
    // pool mechanism causes burst arrivals; interval-method window-boundary errors overestimate rate by ~1%, falsely indicating closed-loop
    // convergence and causing ongoing underruns); nor instantaneous buffer level (callback phase differences drive correction to its limit).
    // Fast R -> positive delta (delay response) -> slow R, and vice versa; the equilibrium automatically compensates fixed host submission
    // overhead (observed ~60us; delta converges near -60us). Playback sinks use it to prevent underrun noise;
    // file sinks (WavFileSink) use it to lock receive rate to nominal sample rate, otherwise recorded files
    // have too few samples per second and play at a changed speed at the nominal sample rate
    auto fmt = sink->current_format();
    auto frame_bytes = static_cast<std::int64_t>(fmt.bits_per_sample / 8) * fmt.channels;
    urb_stat_count++;
    urb_stat_bytes += total_received;
    if (urb_stat_count >= 100) {
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        // Explicit int64_t: duration rep is long long on Android LP64, whereas int64_t
        // is long; mixing auto-inferred types causes clamp type-deduction conflicts (on Windows both
        // are long long, so this does not occur)
        auto time_delta = static_cast<std::int64_t>(now_us - urb_stat_last_time_us);
        auto bytes_delta = static_cast<std::int64_t>(urb_stat_bytes) - urb_stat_last_bytes;
        if (time_delta > 0 && frame_bytes > 0) {
            auto r_hz = bytes_delta * 1000000 / time_delta / frame_bytes;
            auto target_hz = static_cast<std::int64_t>(fmt.sample_rate);
            // Gain: 1% rate difference -> 48us correction per iteration (convergence in about 10 seconds);
            // limit +/-600us (+/-6%) to prevent network spikes sending response delays out of range.
            // Single writer (scheduler thread), multiple readers (network thread loads on submission); use atomic reads/writes
            auto delta = pacing_delta_us.load() + (r_hz - target_hz) * 50 / 1000;
            pacing_delta_us.store(std::clamp(delta, std::int64_t{-600}, std::int64_t{600}));
        }
        urb_stat_count = 0;
        urb_stat_last_bytes = static_cast<std::int64_t>(urb_stat_bytes);
        urb_stat_last_time_us = now_us;
    }
    // Send itself after processing (notification semantics, consistent with handler's other response paths), then report
    // completion: the scheduler advances the serialization point and serves the next URB (synchronous processing; send before reporting)
    responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_received, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
    device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
}

void UacAudioStreamingSinkHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    streaming = false;
    sink->reset();
    // Start closed-loop state from zero: the new connection's buffer level/cadence are independent of the previous one (clear all statistics fields,
    // otherwise remaining byte counts/wall-clock times contaminate the first rate-measurement window after reconnection)
    pacing_delta_us = 0;
    urb_stat_count = 0;
    urb_stat_bytes = 0;
    urb_stat_last_bytes = 0;
    urb_stat_last_time_us = 0;
}

void UacAudioStreamingSinkHandler::on_disconnection(error_code &ec) {
    streaming = false;
    sink->reset();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioStreamingSinkHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        streaming = true;
        sink->reset(); // Clear the buffer to ensure a clean new stream start
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

data_type UacAudioStreamingSinkHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                                std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacDeviceHelper ====================

std::error_code UacDeviceHelper::setup_microphone(std::shared_ptr<UsbDevice> device,
                                                  std::uint8_t ac_interface_number, StringPool &string_pool,
                                                  std::unique_ptr<AudioSource> source,
                                                  const UacDeviceConfig &config) {
    // Locate the adjacent AC/AS interfaces by interface_number (independent of array indices: UAC interfaces in composite devices
    // may not start at the beginning of interfaces and may be interleaved with other functions)
    auto found = device->find_interfaces_by_number<2>(ac_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *ac_interface = found[0];
    auto *as_interface = found[1];

    auto ac = ac_interface->with_handler<UacAudioControlHandler>(string_pool);
    auto as = as_interface->with_handler<UacAudioStreamingSourceHandler>(string_pool, std::move(source));

    // Channel count: infer from source if config does not specify it (0)
    auto resolved = config;
    if (resolved.channels == 0) {
        resolved.channels = as->get_source()->current_format().channels;
    }
    ac->set_config(resolved);
    as->set_ac_handler(ac.get());
    ac->set_as_handler(as.get());
    // The UAC function declares IAD on its AC interface (iFunction = AC's iInterface, as
    // UvcDeviceHelper does; bFirstInterface is filled by interface number when generating the configuration
    // descriptor): in composite devices (e.g. combined UVC+UAC cameras), usbccgp groups interfaces only by IAD;
    // without IAD for the UAC group, AC/AS are split into independent functions and started separately, so usbaudio.sys
    // cannot assemble the AC+AS combination and fails startup (CM_PROB_FAILED_START observed on Windows).
    // Audio has no UVC-style "interface collection" subclass concept; set bFunctionSubClass to 0
    ac_interface->interface_association_descriptor = IadDesc::make(
            2, CC_AUDIO, 0, 0, ac->get_string_interface_value());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // UAC uses isochronous frame scheduling (ISO URB responses delayed at frame cadence); enable the device-level scheduler
    dh->set_use_transfer_scheduler(true);
    dh->setup_interface_handlers();
    return {};
}

std::error_code UacDeviceHelper::setup_speaker(std::shared_ptr<UsbDevice> device, std::uint8_t ac_interface_number,
                                               StringPool &string_pool, std::unique_ptr<AudioSink> sink,
                                               const UacDeviceConfig &config) {
    // Locate the adjacent AC/AS interfaces by interface_number (independent of array indices, as in setup_microphone)
    auto found = device->find_interfaces_by_number<2>(ac_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *ac_interface = found[0];
    auto *as_interface = found[1];

    auto ac = ac_interface->with_handler<UacAudioControlHandler>(string_pool);
    auto as = as_interface->with_handler<UacAudioStreamingSinkHandler>(string_pool, std::move(sink));

    // Channel count: infer from sink if config does not specify it (0)
    auto resolved = config;
    // The speaker factory forces a USB streaming input terminal: AC topology direction is determined by input_terminal_type
    // (build_class_descriptor uses it to identify a speaker). If the caller omits it, microphone topology is generated
    // (IT=mic/OT=USB streaming) with an OUT data endpoint, making Windows usbaudio startup fail
    resolved.input_terminal_type = TT_USB_STREAMING;
    if (resolved.channels == 0) {
        resolved.channels = as->get_sink()->current_format().channels;
    }
    ac->set_config(resolved);
    as->set_ac_handler(ac.get());
    ac->set_as_handler(as.get());
    // The UAC function declares IAD on its AC interface (iFunction = AC's iInterface, as
    // UvcDeviceHelper does; bFirstInterface is filled by interface number when generating the configuration
    // descriptor): in composite devices (e.g. combined UVC+UAC cameras), usbccgp groups interfaces only by IAD;
    // without IAD for the UAC group, AC/AS are split into independent functions and started separately, so usbaudio.sys
    // cannot assemble the AC+AS combination and fails startup (CM_PROB_FAILED_START observed on Windows).
    // Audio has no UVC-style "interface collection" subclass concept; set bFunctionSubClass to 0
    ac_interface->interface_association_descriptor = IadDesc::make(
            2, CC_AUDIO, 0, 0, ac->get_string_interface_value());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // Speaker receive streams (OUT) also require frame scheduling: immediate RET responses make the host (usbip-win vhci's
    // send cadence is driven by RET feedback) think the pipe is always available and send excessively (observed 5x input),
    // so delayed responses limit sending back to 1x. Linux vhci sends by its own frame clock and is unaffected
    dh->set_use_transfer_scheduler(true);
    dh->setup_interface_handlers();
    return {};
}

} // namespace usbipdcpp
