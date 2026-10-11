#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include <algorithm>
#include "usbipdcpp/Session.h"

namespace usbipdcpp {
// ==================== CdcAcmCommunicationInterfaceHandler ====================

CdcAcmCommunicationInterfaceHandler::CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface,
                                                                         StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    // The notification channel's pending buffer is capped at 30 entries: serial-state notifications are infrequent (one per state change),
    // and normal use rarely buffers more than a single-digit number. The 30-entry cap guards against memory accumulation when the host does not read for a long time,
    // while leaving enough headroom not to lose infrequent notifications (drop the oldest when exceeding the limit)
    notification_channel.set_max_pending_messages(30);
}

void CdcAcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<CdcAcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN request
            auto *trx = GenericTransfer::from_handle(transfer.get());
            switch (request) {
                case CdcAcmRequest::GetLineCoding: {
                    auto bytes = line_coding_.to_bytes();
                    trx->data.assign(bytes.begin(), bytes.end());
                    if (setup_packet.length < trx->data.size()) {
                        trx->data.resize(setup_packet.length);
                    }
                    trx->actual_length = trx->data.size();
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM IN request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    trx->actual_length = 0;
                }
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, status, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // OUT request
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto &out_data = trx->data;
            switch (request) {
                case CdcAcmRequest::SetLineCoding: {
                    auto new_coding = LineCoding::from_bytes(out_data);
                    on_set_line_coding(new_coding);
                    line_coding_ = new_coding;
                    SPDLOG_DEBUG("SET_LINE_CODING: baud={}, data_bits={}, stop_bits={}, parity={}",
                                 line_coding_.dwDTERate, line_coding_.bDataBits, line_coding_.bCharFormat,
                                 line_coding_.bParityType);
                    break;
                }
                case CdcAcmRequest::SetControlLineState: {
                    auto state = ControlSignalState::from_uint16(setup_packet.value);
                    on_set_control_line_state(state);
                    control_signal_state_ = state;
                    SPDLOG_DEBUG("SET_CONTROL_LINE_STATE: DTR={}, RTS={}", state.dtr, state.rts);
                    // Notify the data interface of an RTS state change
                    if (data_handler_) {
                        data_handler_->on_rts_changed(state.rts);
                    }
                    break;
                }
                case CdcAcmRequest::SendBreak: {
                    auto duration = setup_packet.value;
                    on_send_break(duration);
                    SPDLOG_DEBUG("SEND_BREAK: duration={}", duration);
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM OUT request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
            // transfer is released automatically on destruction
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
    else {
        // Non-CDC class request; delegate to the subclass
        handle_non_cdc_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                std::move(transfer), ec);
    }
}

void CdcAcmCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                    std::uint32_t transfer_flags,
                                                                    std::uint32_t transfer_buffer_length,
                                                                    TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Handled within the channel: reply immediately if a notification is buffered; otherwise suspend the request until
        // send_serial_state_notification() pushes one
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Interrupt OUT: normally unused by CDC ACM
        // transfer is released automatically on destruction
        SPDLOG_WARN("CDC ACM communication interface received unexpected interrupt OUT");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type CdcAcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // CDC ACM class-specific descriptors
    data_type descriptor;

    // Header Functional Descriptor
    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    // Call Management Functional Descriptor
    // bDataInterface is the data interface number: associated by set_data_handler during assembly, obtained from the data
    // handler's get_interface() (a composite device's communication interface does not start at interface 0; hardcoding
    // 1 prevents the host driver from finding the data interface). Fall back to 1 when unassociated (single-device form COMM=0 -> DATA=1)
    CdcCallManagementDesc{0x05, CS_INTERFACE, 0x01,
                          0x00, // bmCapabilities
                          data_handler_ ? static_cast<std::uint8_t>(data_handler_->get_interface().interface_number) : std::uint8_t{1}}
            .append_to(descriptor);

    // ACM Functional Descriptor
    CdcAcmFunctionalDesc{0x04, CS_INTERFACE, 0x02,
                         0x02} // bmCapabilities: support Set_Line_Coding, Set_Control_Line_State, Send_Break
            .append_to(descriptor);

    // Union Functional Descriptor
    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           get_interface().interface_number, // bMasterInterface: this interface
                           data_handler_ ? static_cast<std::uint8_t>(data_handler_->get_interface().interface_number) : std::uint8_t{1}}
            .append_to(descriptor);

    return descriptor;
}

void CdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::on_send_break(std::uint16_t duration) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::handle_non_cdc_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Return an error by default; subclasses may override to handle non-CDC requests
    SPDLOG_WARN("Unhandled request type 0x{:x} in CDC ACM communication interface", setup_packet.calc_request_type());
    // transfer is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void CdcAcmCommunicationInterfaceHandler::send_serial_state_notification(std::uint16_t state_bits) {
    SerialStateNotification notification;
    notification.data = state_bits;

    // Handled within the channel: reply directly to a pending request; otherwise buffer (limit 1: keep only the newest)
    notification_channel.push(notification.to_bytes());
}

void CdcAcmCommunicationInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    notification_channel.on_new_connection(&current_session);
}

void CdcAcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically), then let the parent clear session
    notification_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's LibusbDeviceHandler
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    // pull callback: when the host requests data and both buffer/queue are empty, call the subclass's on_data_requested
    // to generate data on demand. The channel calls it under its lock (matching the previous semantics of calling under both locks
    // in handle_bulk_transfer), so the callback must not call send_data or similar functions (would deadlock)
    in_channel.set_pull_callback([this](std::uint32_t length) {
        return on_data_requested(static_cast<std::uint16_t>(length));
    });
}

void CdcAcmDataInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void CdcAcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically; wake blocked
    // writers/takers to return as disconnected), then let the parent clear session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // Pending requests are distributed across the IN/OUT channels; try cancelling each in turn
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's LibusbDeviceHandler
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                      std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN: the host requests data. Handled within the channel: reply immediately if data is buffered; otherwise first
        // pull (on_data_requested generates on demand), then suspend the request waiting for send_data if still unavailable
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT: first allow the subclass to consume immediately (e.g. generate a response from the data and push it to IN).
        // true=handled, reply immediately; false=unhandled, suspend the request in the channel (host NAK backpressure);
        // the subclass later takes data with take_out() and replies. When returning false, the subclass must ensure data has not been
        // moved (data remains in the suspended request)
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        if (on_data_received(std::move(trx->data))) {
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
                    seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void CdcAcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // The data interface normally does not handle class-specific control requests
    SPDLOG_WARN("CDC ACM data interface received unexpected control request");
    // transfer is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type CdcAcmDataInterfaceHandler::get_class_specific_descriptor() {
    // The data interface has no class-specific descriptors
    return {};
}

bool CdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    // Default behavior: always consume and reply immediately (subclasses without overrides retain the original always-receive semantics)
    return true;
}

data_type CdcAcmDataInterfaceHandler::on_data_requested(std::uint16_t length) {
    // Return empty by default; subclasses may override
    return {};
}

void CdcAcmDataInterfaceHandler::on_rts_changed(bool rts) {
    // Default empty implementation; subclasses may override
}

// ===== send_data implementation =====

std::size_t CdcAcmDataInterfaceHandler::send_data(const std::uint8_t *data, std::size_t size) {
    // Nonblocking write to the channel (write only available space when full; return 0 on disconnection); internally responds to pending requests
    return in_channel.write_nb(data, size);
}

std::size_t CdcAcmDataInterfaceHandler::send_data(const data_type &data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(data_type &&data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(std::string_view data) {
    return send_data(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const std::uint8_t *data, std::size_t size,
                                                           std::uint32_t timeout_ms) {
    // Blocking channel write: wait for the host to take data when full (timeout_ms=0 waits indefinitely); return the amount written on disconnection
    return in_channel.write(data, size, timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const data_type &data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(data_type &&data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(std::string_view data, std::uint32_t timeout_ms) {
    return send_data_blocking(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), timeout_ms);
}

std::optional<OutEndpointChannel::Pending> CdcAcmDataInterfaceHandler::take_out(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> CdcAcmDataInterfaceHandler::try_take_out() {
    return out_channel.try_take();
}

// ===== Buffer configuration =====

void CdcAcmDataInterfaceHandler::set_tx_buffer_capacity(std::size_t capacity) {
    // The channel's internal lock protects the buffer; watermarks track capacity (default 3/4 and 1/4)
    in_channel.set_capacity(capacity);
    in_high_watermark_ = capacity * 3 / 4;
    in_low_watermark_ = capacity / 4;
}

void CdcAcmDataInterfaceHandler::set_tx_watermarks(std::size_t high, std::size_t low) {
    in_high_watermark_ = high;
    in_low_watermark_ = low;
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_size() const {
    return in_channel.size();
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_available() const {
    return in_channel.available();
}

// ===== Flow-control state =====

void CdcAcmDataInterfaceHandler::set_cts(bool cts) {
    if (comm_handler_) {
        std::uint16_t state = cts ? static_cast<std::uint16_t>(CdcAcmSerialState::CTS) : 0;
        comm_handler_->send_serial_state_notification(state);
    }
}

bool CdcAcmDataInterfaceHandler::get_rts() const {
    if (comm_handler_) {
        return comm_handler_->get_control_signal_state().rts;
    }
    return true; // Allow sending by default
}

void CdcAcmDataInterfaceHandler::set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler) {
    comm_handler_ = handler;
}
} // namespace usbipdcpp
