#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"

#include <algorithm>
#include <cstring>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "spdlog/spdlog.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {

// ==================== UvcStreamingControl ====================

data_type UvcStreamingControl::serialize() const {
    data_type d(SIZE, 0);
    d[0] = static_cast<std::uint8_t>(bmHint & 0xFF);
    d[1] = static_cast<std::uint8_t>((bmHint >> 8) & 0xFF);
    d[2] = bFormatIndex;
    d[3] = bFrameIndex;
    d[4] = static_cast<std::uint8_t>(dwFrameInterval & 0xFF);
    d[5] = static_cast<std::uint8_t>((dwFrameInterval >> 8) & 0xFF);
    d[6] = static_cast<std::uint8_t>((dwFrameInterval >> 16) & 0xFF);
    d[7] = static_cast<std::uint8_t>((dwFrameInterval >> 24) & 0xFF);
    d[8] = static_cast<std::uint8_t>(wKeyFrameRate & 0xFF);
    d[9] = static_cast<std::uint8_t>((wKeyFrameRate >> 8) & 0xFF);
    d[10] = static_cast<std::uint8_t>(wPFrameRate & 0xFF);
    d[11] = static_cast<std::uint8_t>((wPFrameRate >> 8) & 0xFF);
    d[12] = static_cast<std::uint8_t>(wCompQuality & 0xFF);
    d[13] = static_cast<std::uint8_t>((wCompQuality >> 8) & 0xFF);
    d[14] = static_cast<std::uint8_t>(wCompWindowSize & 0xFF);
    d[15] = static_cast<std::uint8_t>((wCompWindowSize >> 8) & 0xFF);
    d[16] = static_cast<std::uint8_t>(wDelay & 0xFF);
    d[17] = static_cast<std::uint8_t>((wDelay >> 8) & 0xFF);
    d[18] = static_cast<std::uint8_t>(dwMaxVideoFrameSize & 0xFF);
    d[19] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 8) & 0xFF);
    d[20] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 16) & 0xFF);
    d[21] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 24) & 0xFF);
    d[22] = static_cast<std::uint8_t>(dwMaxPayloadTransferSize & 0xFF);
    d[23] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 8) & 0xFF);
    d[24] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 16) & 0xFF);
    d[25] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 24) & 0xFF);
    // UVC 1.1 fields (offsets 26–33)
    d[26] = static_cast<std::uint8_t>(dwClockFrequency & 0xFF);
    d[27] = static_cast<std::uint8_t>((dwClockFrequency >> 8) & 0xFF);
    d[28] = static_cast<std::uint8_t>((dwClockFrequency >> 16) & 0xFF);
    d[29] = static_cast<std::uint8_t>((dwClockFrequency >> 24) & 0xFF);
    d[30] = bmFramingInfo;
    d[31] = bPreferredVersion;
    d[32] = bMinVersion;
    d[33] = bMaxVersion;
    // UVC 1.5 fields (offsets 34–47)
    d[34] = bUsage;
    d[35] = bBitDepthLuma;
    d[36] = bmSettings;
    d[37] = bMaxNumberOfRefFramesPlus1;
    d[38] = static_cast<std::uint8_t>(bmRateControlModes & 0xFF);
    d[39] = static_cast<std::uint8_t>((bmRateControlModes >> 8) & 0xFF);
    d[40] = static_cast<std::uint8_t>(bmLayoutPerStream & 0xFF);
    d[41] = static_cast<std::uint8_t>((bmLayoutPerStream >> 8) & 0xFF);
    d[42] = static_cast<std::uint8_t>((bmLayoutPerStream >> 16) & 0xFF);
    d[43] = static_cast<std::uint8_t>((bmLayoutPerStream >> 24) & 0xFF);
    d[44] = static_cast<std::uint8_t>((bmLayoutPerStream >> 32) & 0xFF);
    d[45] = static_cast<std::uint8_t>((bmLayoutPerStream >> 40) & 0xFF);
    d[46] = static_cast<std::uint8_t>((bmLayoutPerStream >> 48) & 0xFF);
    d[47] = static_cast<std::uint8_t>((bmLayoutPerStream >> 56) & 0xFF);
    return d;
}

void UvcStreamingControl::deserialize(const std::uint8_t *data, std::size_t len) {
    if (len < 26)
        return;
    bmHint = static_cast<std::uint16_t>(data[0] | (data[1] << 8));
    bFormatIndex = data[2];
    bFrameIndex = data[3];
    dwFrameInterval = static_cast<std::uint32_t>(data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24));
    wKeyFrameRate = static_cast<std::uint16_t>(data[8] | (data[9] << 8));
    wPFrameRate = static_cast<std::uint16_t>(data[10] | (data[11] << 8));
    wCompQuality = static_cast<std::uint16_t>(data[12] | (data[13] << 8));
    wCompWindowSize = static_cast<std::uint16_t>(data[14] | (data[15] << 8));
    wDelay = static_cast<std::uint16_t>(data[16] | (data[17] << 8));
    dwMaxVideoFrameSize = static_cast<std::uint32_t>(data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24));
    dwMaxPayloadTransferSize =
            static_cast<std::uint32_t>(data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24));
    if (len >= 34) {
        dwClockFrequency = static_cast<std::uint32_t>(data[26] | (data[27] << 8) | (data[28] << 16) | (data[29] << 24));
        bmFramingInfo = data[30];
        bPreferredVersion = data[31];
        bMinVersion = data[32];
        bMaxVersion = data[33];
    }
    if (len >= 48) { // UVC 1.5 extension fields
        bUsage = data[34];
        bBitDepthLuma = data[35];
        bmSettings = data[36];
        bMaxNumberOfRefFramesPlus1 = data[37];
        bmRateControlModes = static_cast<std::uint16_t>(data[38] | (data[39] << 8));
        bmLayoutPerStream =
                static_cast<std::uint64_t>(data[40]) | (static_cast<std::uint64_t>(data[41]) << 8) |
                (static_cast<std::uint64_t>(data[42]) << 16) | (static_cast<std::uint64_t>(data[43]) << 24) |
                (static_cast<std::uint64_t>(data[44]) << 32) | (static_cast<std::uint64_t>(data[45]) << 40) |
                (static_cast<std::uint64_t>(data[46]) << 48) | (static_cast<std::uint64_t>(data[47]) << 56);
    }
}

// ==================== UvcVideoControlHandler ====================

UvcVideoControlHandler::UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void UvcVideoControlHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UvcVideoControlHandler::build_class_descriptor() {
    if (desc_built_)
        return;
    desc_built_ = true;

    // VC Header baInterfaceNr is the VS interface number: associated by the helper's
    // set_vs_handler during assembly, obtained from the VS handler's get_interface() (UVC groups in composite devices
    // do not start at interface 0; hardcoding 1 points to the wrong interface). Fall back to 1 when unassembled (old single-device
    // behavior VC=0 -> VS=1)
    std::uint8_t vs_if_num = vs_handler_ ? static_cast<std::uint8_t>(vs_handler_->get_interface().interface_number) : 1;

    // VC Header(13) + CameraTerminal(18) + PU(13) + OT(9) = 53
    // UVC 1.5: Processing Unit bControlSize=3 + bmVideoStandards(1)
    static constexpr std::uint8_t cam_term_len = 18; // 15 + bControlSize(3)
    static constexpr std::uint8_t pu_len = 13;       // 10 + bControlSize(3) + bmVideoStandards(1)
    data_type d;

    // VC Header (placeholder wTotalLength; fill actual total length after construction)
    VcHeaderDesc{VC_HEADER_1ITF_LEN, CS_INTERFACE, VC_DESC_HEADER,
                 UVC_BCD_1_50, // bcdUVC 0x0150 (UVC 1.5)
                 0x00,
                 0x019BFCC0, // dwClockFrequency = 27000000 (27 MHz)
                 0x01, // bInCollection
                 vs_if_num}
            .append_to(d);

    // Camera Terminal: 18 bytes, bControlSize=3
    VcCameraTerminalDesc{cam_term_len, CS_INTERFACE, VC_DESC_INPUT_TERMINAL,
                         0x01, // bTerminalID
                         ITT_CAMERA,
                         0x00, // bAssocTerminal
                         0x00, // iTerminal
                         0x00, // wObjectiveFocalLengthMin
                         0x00, // wObjectiveFocalLengthMax
                         0x00, // wOcularFocalLength
                         0x03, // bControlSize
                         {0x00, 0x00, 0x00}} // bmControls[3]: no camera terminal controls
            .append_to(d);

    // Processing Unit: 13 bytes, bControlSize=3 + bmVideoStandards (UVC 1.5 Table 3-8)
    VcProcessingUnitDesc{pu_len, CS_INTERFACE, VC_DESC_PROCESSING_UNIT,
                         0x02, // bUnitID
                         0x01, // bSourceID → IT
                         0x00, // wMaxMultiplier
                         0x03, // bControlSize = 3
                         {0x1F, 0x02, 0x00}, // bmControls[3]: Brightness|Contrast|Hue|Saturation|Sharpness|Gain
                         0x00, // iProcessing
                         0x00} // bmVideoStandards: no analog video
            .append_to(d);

    // Output Terminal
    VcOutputTerminalDesc{OUTPUT_TERM_LEN, CS_INTERFACE, VC_DESC_OUTPUT_TERMINAL,
                         0x03, // bTerminalID
                         TT_STREAMING,
                         0x00, // bAssocTerminal
                         0x02, // bSourceID → PU
                         0x00} // iTerminal
            .append_to(d);

    // Fill VC Header wTotalLength (offset 5-6) with the actual descriptor total length
    d[5] = static_cast<std::uint8_t>(d.size() & 0xFF);
    d[6] = static_cast<std::uint8_t>((d.size() >> 8) & 0xFF);

    class_desc_ = std::move(d);
}

data_type UvcVideoControlHandler::get_class_specific_descriptor() {
    return class_desc_;
}

void UvcVideoControlHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Handle class-specific GET_DESCRIPTOR (e.g. CS_INTERFACE=0x24)
    if (request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc_;
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

    // ===== VC interface-level controls (entity=0) =====
    if (entity == ENTITY_VC_INTERFACE) {
        if (control_selector == VC_VIDEO_POWER_MODE_CONTROL) {
            switch (request) {
                case GET_CUR:
                    trx->data = {static_cast<std::uint8_t>(power_on_ ? 1 : 0)};
                    trx->actual_length = 1;
                    break;
                case GET_INFO:
                    trx->data = {0x03}; // GET | SET
                    trx->actual_length = 1;
                    break;
                case SET_CUR:
                    if (!trx->data.empty()) {
                        power_on_ = (trx->data[0] != 0);
                        if (!power_on_ && vs_handler_)
                            vs_handler_->notify_stop_streaming();
                    }
                    trx->actual_length = 0;
                    break;
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        if (control_selector == VC_REQUEST_ERROR_CODE_CONTROL) {
            switch (request) {
                case GET_CUR:
                    trx->data = {0x00}; // no error
                    trx->actual_length = 1;
                    break;
                case GET_INFO:
                    trx->data = {0x01}; // GET only
                    trx->actual_length = 1;
                    break;
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // entity != 0: TinyUSB returns success directly for entities present in the descriptors (ACK, no data)
    // IT=0x01, OT=0x03 have no actual control logic, but require ACK to prevent Windows STALL
    if (entity == ENTITY_INPUT_TERMINAL || entity == ENTITY_OUTPUT_TERMINAL) {
        trx->actual_length = 0;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, std::move(transfer)));
        return;
    }

    // ===== Processing Unit(entity=2)=====
    if (entity != ENTITY_PROCESSING_UNIT) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    struct PuControl {
        std::uint8_t selector;
        std::int16_t cur, min, max, def, step;
    };
    static const PuControl controls[] = {
            {PU_BRIGHTNESS, 128, 0, 255, 128, 1}, {PU_CONTRAST, 128, 0, 255, 128, 1}, {PU_HUE, 0, -180, 180, 0, 1},
            {PU_SATURATION, 128, 0, 255, 128, 1}, {PU_SHARPNESS, 2, 0, 15, 2, 1},     {PU_GAIN, 64, 0, 255, 64, 1},
    };

    const PuControl *ctrl = nullptr;
    for (auto &c: controls) {
        if (c.selector == control_selector) {
            ctrl = &c;
            break;
        }
    }

    std::int16_t val = 0;

    switch (request) {
        case GET_CUR:
            val = ctrl ? ctrl->cur : 0;
            goto respond_16;
        case GET_MIN:
            val = ctrl ? ctrl->min : 0;
            goto respond_16;
        case GET_MAX:
            val = ctrl ? ctrl->max : 0;
            goto respond_16;
        case GET_DEF:
            val = ctrl ? ctrl->def : 0;
            goto respond_16;
        case GET_RES:
            val = ctrl ? ctrl->step : 1;
            goto respond_16;
        respond_16:
            trx->data = {static_cast<std::uint8_t>(val & 0xFF), static_cast<std::uint8_t>((val >> 8) & 0xFF)};
            trx->actual_length = 2;
            break;
        case GET_LEN:
            trx->data = {0x02, 0x00};
            trx->actual_length = 2;
            break;
        case GET_INFO:
            trx->data = {static_cast<std::uint8_t>(ctrl ? 0x03 : 0x00)};
            trx->actual_length = 1;
            break;
        case SET_CUR:
            trx->actual_length = 0;
            break;
        default:
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
    }

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
}

void UvcVideoControlHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       std::error_code &ec) {
    if (ep.is_in()) {
        // Handled within the channel: reply immediately if a state notification is buffered; otherwise suspend waiting for send_vc_status()
        status_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UvcVideoControlHandler::send_vc_status(data_type status) {
    // Push to the channel: reply directly to pending requests; otherwise buffer and wait (state events are infrequent, so no limit)
    status_channel.push(std::move(status));
}

void UvcVideoControlHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    status_channel.on_new_connection(&current_session);
}

void UvcVideoControlHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically), then let the parent clear session
    status_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UvcVideoControlHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = status_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's LibusbDeviceHandler
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

data_type UvcVideoControlHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc_;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UvcVideoStreamingHandler ====================

UvcVideoStreamingHandler::UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                   std::unique_ptr<VideoSource> source) :
    VirtualInterfaceHandler(handle_interface, string_pool), source_(std::move(source)) {
    probe_data_.dwMaxVideoFrameSize = static_cast<std::uint32_t>(source_->max_frame_size());
    probe_data_.dwMaxPayloadTransferSize = 512; // wMaxPacketSize of ISO endpoint
    probe_data_.dwFrameInterval = source_->frame_interval();
    frame_interval_ = std::chrono::microseconds(source_->frame_interval() / 10); // 100ns → µs
}

void UvcVideoStreamingHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UvcVideoStreamingHandler::build_class_descriptor() {
    auto fmt = source_->current_format();

    std::uint8_t ep_addr = 0x81;
    // Get the endpoint address from streaming alt (alt 1)
    if (handle_interface.endpoints.size() > 1 && !handle_interface.endpoints[1].empty())
        ep_addr = handle_interface.endpoints[1][0].address;
    else if (!handle_interface.endpoints.empty() && !handle_interface.endpoints[0].empty())
        ep_addr = handle_interface.endpoints[0][0].address;

    auto cat = uvc_format_category(fmt.fourcc);
    auto guid = UvcGuid::from_fourcc(fmt.fourcc);
    auto bpp = static_cast<std::uint8_t>(fmt.bits_per_pixel);

    std::uint8_t format_subtype{};
    std::uint8_t frame_subtype{};
    data_type format;

    if (cat == UvcFormatCategory::Uncompressed) {
        format_subtype = VS_DESC_FORMAT_UNCOMPRESSED;
        frame_subtype = VS_DESC_FRAME_UNCOMPRESSED;
        // Format descriptor: 27 bytes (retain exactly the same layout as before the fix)
        format.resize(27, 0);
        format[0] = 27;
        format[1] = 0x24;
        format[2] = format_subtype;
        format[3] = 0x01;
        format[4] = 0x01;
        std::memcpy(&format[5], guid.data, 16);
        format[21] = bpp;
        format[22] = 0x01;
        format[23] = 0x00;
        format[24] = 0x00;
    } else if (cat == UvcFormatCategory::Mjpeg) {
        format_subtype = VS_DESC_FORMAT_MJPEG;
        frame_subtype = VS_DESC_FRAME_MJPEG;
        // MJPEG Format descriptor: 11 bytes(UVC 1.5 MJPEG Payload Table 3-1)
        format.resize(11, 0);
        format[0] = 11;
        format[1] = 0x24;
        format[2] = format_subtype;
        format[3] = 0x01;
        format[4] = 0x01;
        format[5] = 0x01; // bmFlags
        format[6] = 0x01; // bDefaultFrameIndex
    } else if (cat == UvcFormatCategory::FrameBased) {
        format_subtype = VS_DESC_FORMAT_FRAME_BASED;
        frame_subtype = VS_DESC_FRAME_FRAME_BASED;
        // Frame-Based Format: 28 bytes(UVC 1.5 Frame-Based Payload Table 3-1)
        format.resize(28, 0);
        format[0] = 28;
        format[1] = 0x24;
        format[2] = format_subtype;
        format[3] = 0x01;
        format[4] = 0x01;
        std::memcpy(&format[5], guid.data, 16);
        format[21] = bpp;
        format[22] = 0x01;
        format[23] = 0x00;
        format[24] = 0x00;
        format[27] = 0x01; // bVariableSize
    } else { // H264
        format_subtype = VS_DESC_FORMAT_H264;
        frame_subtype = VS_DESC_FRAME_H264;
        // H.264 Format: 52 bytes(UVC 1.5 H.264 Payload Table 3-1)
        format.resize(52, 0);
        format[0] = 52;
        format[1] = 0x24;
        format[2] = format_subtype;
        format[3] = 0x01;
        format[4] = 0x01;
        format[5] = 0x01; // bDefaultFrameIndex
        std::memcpy(&format[21], guid.data, 16);
        format[37] = bpp;
        format[38] = 0x01;
        format[39] = 0x01;
    }

    // Frame descriptor (Uncompressed/MJPEG/FrameBased: 26 base+12 intervals=38; H264: 44 base+12 intervals=56)
    auto fps_val = 10'000'000ULL / fmt.default_frame_interval;
    auto bit_rate = static_cast<std::uint32_t>(fmt.max_frame_size * 8 * fps_val);
    auto min_iv = fmt.min_frame_interval;
    auto max_iv = fmt.max_frame_interval;
    auto step_iv = min_iv;

    auto frame_base = (cat == UvcFormatCategory::H264) ? VS_FRM_H264_BASE_LEN : 26u;
    auto frame_len = frame_base + 12;
    data_type frame(frame_len, 0);
    frame[0] = static_cast<std::uint8_t>(frame_len);
    frame[1] = 0x24;
    frame[2] = frame_subtype;
    frame[3] = 0x01; // bFrameIndex
    frame[4] = 0x00; // bmCapabilities
    frame[5] = static_cast<std::uint8_t>(fmt.width & 0xFF);
    frame[6] = static_cast<std::uint8_t>((fmt.width >> 8) & 0xFF);
    frame[7] = static_cast<std::uint8_t>(fmt.height & 0xFF);
    frame[8] = static_cast<std::uint8_t>((fmt.height >> 8) & 0xFF);
    for (int i = 0; i < 4; ++i) {
        frame[9 + i] = static_cast<std::uint8_t>((bit_rate >> (i * 8)) & 0xFF);
        frame[13 + i] = static_cast<std::uint8_t>((bit_rate >> (i * 8)) & 0xFF);
    }
    frame[17] = static_cast<std::uint8_t>(fmt.max_frame_size & 0xFF);
    frame[18] = static_cast<std::uint8_t>((fmt.max_frame_size >> 8) & 0xFF);
    frame[19] = static_cast<std::uint8_t>((fmt.max_frame_size >> 16) & 0xFF);
    frame[20] = static_cast<std::uint8_t>((fmt.max_frame_size >> 24) & 0xFF);
    frame[21] = static_cast<std::uint8_t>(fmt.default_frame_interval & 0xFF);
    frame[22] = static_cast<std::uint8_t>((fmt.default_frame_interval >> 8) & 0xFF);
    frame[23] = static_cast<std::uint8_t>((fmt.default_frame_interval >> 16) & 0xFF);
    frame[24] = static_cast<std::uint8_t>((fmt.default_frame_interval >> 24) & 0xFF);
    frame[25] = 0x00; // bFrameIntervalType = 0 (continuous)
    // usbvideo.sys!DumpAndValidateFrameUncompressed has three divisibility checks for continuous frame intervals:
    //   if (step != 0) {
    //       if (max <= min)                → STATUS_INVALID_PARAMETER
    //       if ((max - min) % step != 0)   → STATUS_INVALID_PARAMETER
    //   }
    // thus max must be min + N*step (positive integer N), otherwise Windows directly reports Code 10.
    // min/max/step all come from VideoSource; step=min ensures (max-min) is an integral multiple of min.
    for (int i = 0; i < 4; ++i)
        frame[frame_base + i] = static_cast<std::uint8_t>((min_iv >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i)
        frame[frame_base + 4 + i] = static_cast<std::uint8_t>((max_iv >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i)
        frame[frame_base + 8 + i] = static_cast<std::uint8_t>((step_iv >> (i * 8)) & 0xFF);

    // Color Matching: mandatory for Uncompressed and MJPEG (their respective payload spec section 3)
    bool has_color = (cat == UvcFormatCategory::Uncompressed || cat == UvcFormatCategory::Mjpeg);
    data_type color;
    if (has_color) {
        VsColorMatchingDesc{VS_COLOR_MATCHING_LEN, CS_INTERFACE, VS_DESC_COLORFORMAT,
                            VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M}
                .append_to(color);
    }

    // VS Input Header (placeholder wTotalLength; fill actual total length after concatenation)
    static constexpr std::uint8_t bControlSize = 1;

    data_type header;
    VsInputHeaderDesc{VS_INPUT_HEADER_LEN, CS_INTERFACE, VS_DESC_INPUT_HEADER,
                      0x01, // bNumFormats
                      0x00,
                      ep_addr,
                      0x00, // bmInfo
                      0x03, // bTerminalLink → Output Terminal ID 3
                      0x00, // bStillCaptureMethod
                      0x00, // bTriggerSupport
                      0x00, // bTriggerUsage
                      bControlSize,
                      0x00} // bmaControls[0]
            .append_to(header);

    data_type d;
    d.insert(d.end(), header.begin(), header.end());
    d.insert(d.end(), format.begin(), format.end());
    d.insert(d.end(), frame.begin(), frame.end());
    d.insert(d.end(), color.begin(), color.end());

    // Fill VS Input Header wTotalLength (offset 4-5) with the actual descriptor total length
    d[4] = static_cast<std::uint8_t>(d.size() & 0xFF);
    d[5] = static_cast<std::uint8_t>((d.size() >> 8) & 0xFF);

    class_desc_ = std::move(d);
}

data_type UvcVideoStreamingHandler::get_class_specific_descriptor() {
    return class_desc_;
}

void UvcVideoStreamingHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    auto ctrl_code = setup_packet.value >> 8;

    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Handle class-specific GET_DESCRIPTOR (e.g. CS_INTERFACE=0x24)
    if (request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc_;
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

    if (!(ctrl_code == VS_PROBE_CONTROL || ctrl_code == VS_COMMIT_CONTROL ||
          ctrl_code == VS_STREAM_ERROR_CODE_CONTROL)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // Stream Error Code Control
    if (ctrl_code == VS_STREAM_ERROR_CODE_CONTROL) {
        switch (request) {
            case GET_CUR:
                trx->data = {0x00}; // no error
                trx->actual_length = 1;
                break;
            case GET_INFO:
                trx->data = {0x01}; // GET only
                trx->actual_length = 1;
                break;
            default:
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        return;
    }

    bool is_commit = (ctrl_code == VS_COMMIT_CONTROL);

    if (request == GET_CUR || request == GET_MIN || request == GET_MAX || request == GET_DEF) {
        auto fmt = source_->current_format();
        auto max_frame_size = static_cast<std::uint32_t>(source_->max_frame_size());
        auto ctrl = probe_data_;
        // UVC 1.5 section 4.3.1.1 Table 4-75: the following fields are device capabilities; host cannot modify them
        ctrl.bmFramingInfo = 0x03; // D0: FID required, D1: EOF optional
        ctrl.bPreferredVersion = 1; // Preferred payload format version (UVC 1.5 offset 31)
        ctrl.bMinVersion = 1;       // Minimum supported version (offset 32)
        ctrl.bMaxVersion = 1;       // Maximum supported version (offset 33)
        ctrl.dwClockFrequency = 27000000; // 27MHz, offset 26
        // Uncompressed video: compression-related fields must be 0, or they contradict VS Input Header bmaControls=0
        ctrl.wKeyFrameRate = 0;
        ctrl.wPFrameRate = 0;
        ctrl.wCompQuality = 0;
        ctrl.wCompWindowSize = 0;
        ctrl.wDelay = 0;
        auto interval = fmt.default_frame_interval;
        auto min_iv = fmt.min_frame_interval;
        auto max_iv = fmt.max_frame_interval;
        // UVC 1.5 Table 4-76: GET_MIN returns minimum values of all negotiation fields
        if (request == GET_MIN) {
            // Nonzero prevents the host treating a field as unset and triggering fix-up (libuvc stream.c:273)
            ctrl.dwMaxVideoFrameSize = 1;
            // UVC 1.5 section 4.3.1.1 offset 22: maximum bytes per payload transfer; 1 is the minimum valid value
            ctrl.dwMaxPayloadTransferSize = 1;
            ctrl.dwFrameInterval = max_iv; // Longest frame interval = lowest data rate
        } else if (request == GET_MAX) {
            ctrl.dwMaxVideoFrameSize = max_frame_size;
            ctrl.dwMaxPayloadTransferSize = max_frame_size;
            ctrl.dwFrameInterval = min_iv; // Shortest frame interval = highest frame rate
        } else if (request == GET_DEF) {
            ctrl.bFormatIndex = 1;
            ctrl.bFrameIndex = 1;
            ctrl.dwFrameInterval = interval;
            ctrl.dwMaxVideoFrameSize = max_frame_size;
            // UVC 1.5 section 4.3.1.1 Table 4-75 offset 22: device-defined, host read-only, mandatory support.
            // Use ISO endpoint wMaxPacketSize (512), without aligning to max_frame_size
            ctrl.dwMaxPayloadTransferSize = 512;
        } else {
            // GET_CUR: use the last negotiated value, or default if never negotiated
            if (ctrl.dwMaxVideoFrameSize == 0)
                ctrl.dwMaxVideoFrameSize = max_frame_size;
            if (ctrl.dwFrameInterval == 0)
                ctrl.dwFrameInterval = interval;
        }
        auto resp = ctrl.serialize();
        auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
        trx->data.assign(resp.begin(), resp.begin() + act_len);
        trx->actual_length = act_len;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == GET_LEN) {
        std::uint16_t len = UvcStreamingControl::SIZE;
        trx->data = {static_cast<std::uint8_t>(len & 0xFF), static_cast<std::uint8_t>((len >> 8) & 0xFF)};
        trx->actual_length = 2;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == GET_INFO) {
        trx->data = {0x03}; // GET | SET
        trx->actual_length = 1;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == SET_CUR && !is_commit) {
        if (trx->data.size() >= 26) {
            probe_data_.deserialize(trx->data.data(), trx->data.size());
            // UVC 1.5 section 4.3.1.1 Table 4-75: device capability fields; host cannot modify them
            probe_data_.bmFramingInfo = 0x03;          // offset 30: FID+EOF
            probe_data_.bPreferredVersion = 1;          // offset 31
            probe_data_.bMinVersion = 1;                // offset 32
            probe_data_.bMaxVersion = 1;                // offset 33
            probe_data_.dwClockFrequency = 27000000;    // offset 26: 27MHz
            // UVC 1.5 Table 4-75 offset 34-47: temporal encoding fields valid only for H.264; clear others
            if (uvc_format_category(source_->current_format().fourcc) != UvcFormatCategory::H264) {
                probe_data_.bUsage = 0;
                probe_data_.bBitDepthLuma = 0;
                probe_data_.bmSettings = 0;
                probe_data_.bMaxNumberOfRefFramesPlus1 = 0;
                probe_data_.bmRateControlModes = 0;
                probe_data_.bmLayoutPerStream = 0;
            }
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
    else if (request == SET_CUR && is_commit) {
        if (trx->data.size() >= 26) {
            probe_data_.deserialize(trx->data.data(), trx->data.size());
            // UVC 1.5 section 4.3.1.1 Table 4-75: as with PROBE SET_CUR, write device state
            probe_data_.bmFramingInfo = 0x03;
            probe_data_.bPreferredVersion = 1;
            probe_data_.bMinVersion = 1;
            probe_data_.bMaxVersion = 1;
            probe_data_.dwClockFrequency = 27000000;
            if (uvc_format_category(source_->current_format().fourcc) != UvcFormatCategory::H264) {
                probe_data_.bUsage = 0;
                probe_data_.bBitDepthLuma = 0;
                probe_data_.bmSettings = 0;
                probe_data_.bMaxNumberOfRefFramesPlus1 = 0;
                probe_data_.bmRateControlModes = 0;
                probe_data_.bmLayoutPerStream = 0;
            }
        }
        auto fmt = source_->current_format();
        source_->set_format(fmt.fourcc, fmt.width, fmt.height, probe_data_.dwFrameInterval);
        committed_ = true;
        frame_buffer_.resize(source_->max_frame_size());
        frame_offset_ = 0;
        current_fid_ = false;
        // Restart streaming: reset frame clock; frame interval may have changed after set_format
        frame_started_at_ = {};
        frame_interval_ = std::chrono::microseconds(source_->frame_interval() / 10);
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
    else {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UvcVideoStreamingHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                           std::uint32_t transfer_flags,
                                                           std::uint32_t transfer_buffer_length,
                                                           TransferHandle transfer, int num_iso_packets,
                                                           std::error_code &ec) {
    if (!streaming_ || !committed_) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // Kernel usb_submit_urb initializes iso_frame_desc[n].status to -EXDEV;
    // clear it to zero, or the kernel UVC driver skips every packet.
    for (auto &iso: iso_descs)
        iso.status = 0;

    // Frame clock: frame rate is determined by the frame clock and does not drift with host consumption speed.
    // Frame transmitted before frame interval expires -> empty packets until next frame (0-byte isochronous packets are valid and skipped by the driver),
    // preventing accelerated playback when the host pulls quickly.
    // Note: virtual devices do not simulate USB-bus isochronous transaction cadence (real cameras are bandwidth-limited by 125us x packet
    // size; virtual devices are not). Respond to all URBs immediately; data is transferred as fast as the host pulls,
    // so even large frames finish within the frame interval, keeping complete frames in real time. Audio cannot do this
    // (sample rate needs precise throttling), but video frame data is bounded and the frame clock already controls frame rate, so immediate
    // responses cannot run out of control.
    // If a frame cannot finish within its interval (slow host consumption), do not switch frames; finish the current frame before pulling the next
    // (the host driver marks partial frames corrupted and discards the entire frame; prefer slow to fragmented)
    const auto now = std::chrono::steady_clock::now();
    if (frame_interval_.count() > 0 && frame_offset_ == 0 && now < frame_started_at_ + frame_interval_) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, 0,
                static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
        return;
    }

    if (frame_offset_ == 0) {
        VideoFrame vf{};
        if (!source_->get_frame(vf)) {
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }
        frame_buffer_.assign(vf.data, vf.data + vf.size);
        current_fid_ = !current_fid_;
        frame_started_at_ = now;
    }

    std::uint32_t total_sent = 0;
    std::size_t frame_remaining = frame_buffer_.size() - frame_offset_;

    for (int i = 0; i < num_iso_packets && frame_remaining > 0; ++i) {
        auto &iso = iso_descs[i];
        if (iso.length < UVC_PAYLOAD_HEADER_SIZE) {
            iso.actual_length = 0;
            continue;
        }

        auto chunk = std::min(static_cast<std::size_t>(iso.length - UVC_PAYLOAD_HEADER_SIZE), frame_remaining);

        std::uint8_t header_info = current_fid_ ? UVC_PAYLOAD_HEADER_FID : 0;
        // Send EOF bit (D7) as specified by UVC 1.5 Table 2-5: Windows usbvideo.sys uses it to delimit frames.
        // Linux uvcvideo interprets a custom bit (EOF=0x40) and cannot recognize it, but forcibly delimits frames when buffers fill as fallback; behavior is unchanged
        if (frame_offset_ + chunk >= frame_buffer_.size())
            header_info |= UVC_PAYLOAD_HEADER_EOF; // EOF

        auto *dst = &data[iso.offset];
        dst[0] = UVC_PAYLOAD_HEADER_SIZE;
        dst[1] = header_info;
        std::memcpy(dst + UVC_PAYLOAD_HEADER_SIZE, frame_buffer_.data() + frame_offset_, chunk);

        iso.actual_length = static_cast<std::uint32_t>(UVC_PAYLOAD_HEADER_SIZE + chunk);
        total_sent += iso.actual_length;
        frame_offset_ += chunk;
        frame_remaining -= chunk;
    }

    if (frame_offset_ >= frame_buffer_.size())
        frame_offset_ = 0;

    // Respond immediately (bypass TransferScheduler's 125us x packet isochronous throttling): virtual devices
    // have no real bus, so data transfers as fast as the host pulls; large frames must finish within the frame interval
    // for smooth video. The frame clock already caps frame rate, preventing runaway behavior
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_sent, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
}

void UvcVideoStreamingHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    committed_ = false;
    streaming_ = false;
    frame_offset_ = 0;
    current_fid_ = false;
    frame_started_at_ = {}; // Reset frame clock: the first frame starts immediately when streaming resumes after reconnection
}

void UvcVideoStreamingHandler::on_disconnection(error_code &ec) {
    streaming_ = false;
    committed_ = false;
    frame_started_at_ = {};
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UvcVideoStreamingHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming_ = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        if (committed_) {
            streaming_ = true;
            frame_offset_ = 0;
            current_fid_ = false;
        }
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint8_t UvcVideoStreamingHandler::request_get_interface(std::uint32_t *p_status) {
    return streaming_ ? 1 : 0;
}

data_type UvcVideoStreamingHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                            std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc_;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UvcDeviceHelper ====================

std::error_code UvcDeviceHelper::setup(std::shared_ptr<UsbDevice> device, std::uint8_t vc_interface_number,
                                       StringPool &string_pool, std::unique_ptr<VideoSource> source) {
    // Locate the adjacent VC/VS interfaces by interface_number (independent of array indices: UVC interfaces in composite devices
    // may not start at the beginning of interfaces and may be interleaved with other functions)
    auto found = device->find_interfaces_by_number<2>(vc_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *vc_interface = found[0];
    auto *vs_interface = found[1];

    auto vc = vc_interface->with_handler<UvcVideoControlHandler>(string_pool);
    auto vs = vs_interface->with_handler<UvcVideoStreamingHandler>(string_pool, std::move(source));

    vc->set_vs_handler(vs.get());
    vs->set_vc_handler(vc.get());

    // VC/VS use the same iInterface: USBCCGP removes IAD from the child PDO configuration descriptor,
    // and usbvideo.sys relies on matching iInterface to recognize VC and VS as one function
    vs->sync_string_interface_from(*vc);

    // IAD: declared by the VC interface for the UVC function (iFunction must equal VC iInterface,
    // UVC 1.5 Table 3-1; bFirstInterface is filled by interface number when generating the configuration descriptor)
    vc_interface->interface_association_descriptor = IadDesc::make(
            2, CC_VIDEO, SC_VIDEO_INTERFACE_COLLECTION, 0, vc->get_string_interface_value());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // UVC isochronous URBs respond immediately (virtual devices do not simulate bus isochronous transaction cadence; the frame clock already controls
    // frame rate), without enabling the device-level transfer scheduler (off by default; enabling it throttles bandwidth by 125us x packets,
    // making large-frame video play slowly). UNLINK uses the channel's own cancel_pending
    dh->setup_interface_handlers();
    return {};
}

} // namespace usbipdcpp
