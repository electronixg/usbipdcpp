#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"

// ========== Interrupt transfer handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                      std::uint32_t transfer_flags,
                                                                      std::uint32_t transfer_buffer_length,
                                                                      TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Interrupt IN: the host requests an input report
        // First let the subclass generate a report on demand (pull model); the subclass may call send_input_report()
        // to push data into the channel, after which the normal push flow applies.
        // Must call outside the lock: the subclass implementation may call send_input_report()
        on_input_report_requested(transfer_buffer_length);

        // Handled within the channel: reply immediately if a report is buffered; otherwise suspend waiting for send_input_report()
        input_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Interrupt OUT: the host sends an output report
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_output_report_received(asio::buffer(trx->data));

        // transfer is released automatically on destruction
        responder->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

// ========== Sending input reports ==========

void usbipdcpp::HidVirtualInterfaceHandler::send_input_report(asio::const_buffer data) {
    // Push to the channel: reply directly to pending requests; otherwise buffer and wait (drop oldest on overflow; see
    // MAX_PENDING_INPUT_REPORTS)
    input_channel.push(data_type(static_cast<const std::uint8_t *>(data.data()),
                                 static_cast<const std::uint8_t *>(data.data()) + data.size()));
}

// ========== Default callback implementations ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    // Default empty implementation; subclasses may override
}

void usbipdcpp::HidVirtualInterfaceHandler::on_output_report_received(asio::const_buffer data) {
    // Default empty implementation; subclasses may override
}

// ========== Connection lifecycle ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    input_channel.on_new_connection(&current_session);
}

void usbipdcpp::HidVirtualInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically), then let the parent clear session
    input_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

// ========== UNLINK handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum,
                                                                 std::uint32_t cmd_seqnum) {
    bool cancelled = input_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches kernel stub_tx.c (RET_UNLINK carries urb->status=-ECONNRESET when priv->unlinking,
    // otherwise 0) and usbipd-libusb; also the unlink pattern used by this project's LibusbDeviceHandler
    // (trxstat2error(CANCELLED)=-ECONNRESET; return 0 when not found)
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ========== Control request handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    switch (type) {
        case RequestType::Class: {
            auto request = static_cast<HIDRequest>(setup_packet.request);
            std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            if (!setup_packet.is_out()) {
                data_type result;
                switch (request) {
                    case HIDRequest::GetIdle: {
                        result = request_get_idle(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                  &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    case HIDRequest::GetProtocol: {
                        auto ret = request_get_protocol(&status);
                        // Control-transfer data stages are uniformly little-endian (GetProtocol returns 1 byte,
                        // so endianness makes no difference; consistent with GetStatus and others)
                        vector_append_to_le(result, ret);
                        break;
                    }
                    case HIDRequest::GetReport: {
                        result = request_get_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                    &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // Write data into transfer_handle
                trx->data = std::move(result);
                trx->actual_length = trx->data.size();
                trx->data_offset = 0;

                responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            }
            else {
                // Get OUT data from transfer_handle
                data_type out_data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
                switch (request) {
                    case HIDRequest::SetIdle: {
                        request_set_idle(setup_packet.value >> 8, &status);
                        break;
                    }
                    case HIDRequest::SetProtocol: {
                        request_set_protocol(setup_packet.value, &status);
                        break;
                    }
                    case HIDRequest::SetReport: {
                        request_set_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length, out_data,
                                           &status);
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // transfer is released automatically on destruction
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
            }
            break;
        }
        default: {
            handle_non_hid_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                    std::move(transfer), ec);
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_descriptor(std::uint8_t type,
                                                                                   std::uint8_t language_id,
                                                                                   std::uint16_t descriptor_length,
                                                                                   std::uint32_t *p_status) {
    auto hid_type = static_cast<HidDescriptorType>(type);
    switch (hid_type) {
        case HidDescriptorType::Report: {
            return get_report_descriptor();
        }
        default: {
            SPDLOG_ERROR("Unimplement descriptor type: {:x}", static_cast<std::uint32_t>(hid_type));
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            return {};
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::get_class_specific_descriptor() {
    auto report_descriptor_size = get_report_descriptor_size();
    return {
            0x09, // bLength
            HidDescriptorType::Hid, // bDescriptorType: HID
            0x11,
            0x01, // bcdHID 1.11
            0x00, // bCountryCode
            0x01, // bNumDescriptors
            HidDescriptorType::Report, // bDescriptorType[0] HID
            static_cast<std::uint8_t>(report_descriptor_size),
            static_cast<std::uint8_t>(report_descriptor_size >> 8), // wDescriptorLength[0]
    };
}

// ========== Default non-HID request implementation ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_hid_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // transfer is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// ========== Default report request implementations ==========

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_report(std::uint8_t type,
                                                                               std::uint8_t report_id,
                                                                               std::uint16_t length,
                                                                               std::uint32_t *p_status) {
    // Nominal response (matching kernel f_hid.c hidg_setup: GET_REPORT returns an all-zero empty report,
    // with length determined by host wLength). Actual report data uses the interrupt IN endpoint; control-transfer
    // GET_REPORT is only a fallback response. Subclasses needing actual data should override
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    return data_type(length, 0);
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id,
                                                               std::uint16_t length, const data_type &data,
                                                               std::uint32_t *p_status) {
    // Match kernel f_hid.c ssreport mode (without an OUT endpoint, SET_REPORT is received through ep0,
    // with data stored in set_report_buf for userspace reads). This project's default has no consumer; accept and
    // discard. Subclasses needing output reports (e.g. keyboard LEDs) should override this function
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
}
