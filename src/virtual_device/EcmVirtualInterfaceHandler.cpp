#include "usbipdcpp/virtual_device/EcmVirtualInterfaceHandler.h"

#include <cstdio>
#include <string>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/EcmConstants.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {

// ==================== EcmCommunicationInterfaceHandler ====================

EcmCommunicationInterfaceHandler::EcmCommunicationInterfaceHandler(UsbInterface &handle_interface,
                                                                   StringPool &string_pool,
                                                                   std::array<std::uint8_t, 6> mac_address) :
    VirtualInterfaceHandler(handle_interface, string_pool), interface_number_(handle_interface.interface_number) {
    // Convert the MAC address to 12 hex characters in a string descriptor (the cdc_ether host driver parses this format,
    // requiring uppercase Unicode 0-9A-F characters, exactly 12 in length)
    char hex[13];
    std::snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X", mac_address[0], mac_address[1], mac_address[2],
                  mac_address[3], mac_address[4], mac_address[5]);
    mac_string_index_ = string_pool.new_string(std::wstring(hex, hex + 12));
    // The notification channel's pending buffer is capped at 30 entries: network-state notifications are infrequent (one per connection/rate change),
    // and normal use rarely buffers more than a single-digit number. The 30-entry cap guards against memory accumulation when the host does not read for a long time,
    // while leaving enough headroom not to lose infrequent notifications (drop the oldest on overflow, matching the CdcAcm notification channel)
    notification_channel.set_max_pending_messages(30);
}

UsbInterface EcmCommunicationInterfaceHandler::make_interface(std::uint8_t interrupt_in_ep) {
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
            .interface_subclass = 0x06, // Ethernet Networking Control Model
            .interface_protocol = 0x00,
            .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                       .max_packet_size = 16, // ECM_STATUS_BYTECOUNT: accommodate header+SPEED_CHANGE data
                                       .interval = 32}}}, // Match kernel ECM_STATUS_INTERVAL_MS=32ms (Full speed)
    };
    // IAD: ECM is a two-interface function (communication+data), matching kernel f_ecm.c's ecm_iad_descriptor
    // (bFunctionClass=COMM,bFunctionSubClass=ETHERNET(0x06),bFunctionProtocol=NONE,
    // bInterfaceCount=2). With bDeviceClass=0x02, the Windows class driver binds the whole device without consulting IAD,
    // but the descriptor matches the kernel, and Windows composite-device recognition in multifunction configurations also requires it.
    // bFirstInterface is filled from its owning interface's interface_number when generating the configuration descriptor
    i.interface_association_descriptor = IadDesc::make(
            2, static_cast<std::uint8_t>(ClassCode::CDC), 0x06 /* USB_CDC_SUBCLASS_ETHERNET */);
    return i;
}

void EcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<EcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN requests: statistics/power filters are declared unsupported in the descriptors; match kernel
            // f_ecm.c's default (STALL) by returning EPIPE for all
            SPDLOG_WARN("Unsupported ECM IN request 0x{:x}", setup_packet.request);
            status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
        else {
            // OUT requests
            switch (request) {
                case EcmRequest::SetEthernetPacketFilter: {
                    // ECM120 section 6.2.4: no data; wValue=filter bitmap, wIndex=interface number.
                    // Match kernel ecm_setup: wLength != 0 is invalid (STALL)
                    if (setup_packet.length != 0) {
                        SPDLOG_WARN("SET_ETHERNET_PACKET_FILTER with data, wLength={}", setup_packet.length);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                        break;
                    }
                    packet_filter_ = setup_packet.value;
                    on_set_packet_filter(packet_filter_);
                    SPDLOG_DEBUG("SET_ETHERNET_PACKET_FILTER: 0x{:04x}", packet_filter_);
                    break;
                }
                default: {
                    // Multicast filtering/power-pattern filtering: declared unsupported in descriptors; match kernel STALL
                    SPDLOG_WARN("Unsupported ECM OUT request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
        }
        // transfer is released automatically on destruction
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, status, 0));
    }
    else {
        // Non-class request (standard requests were already dispatched by the base class; this is the fallback)
        SPDLOG_WARN("Unhandled request type 0x{:x} in ECM communication interface", setup_packet.calc_request_type());
        // transfer is released automatically on destruction
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void EcmCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                 std::uint32_t transfer_flags,
                                                                 std::uint32_t transfer_buffer_length,
                                                                 TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Handled within the channel: reply immediately if a notification is buffered; otherwise suspend the request until
        // send_network_connection / send_speed_change pushes one
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Interrupt OUT: unused by ECM (matching CdcAcm handling)
        // transfer is released automatically on destruction
        SPDLOG_WARN("ECM communication interface received unexpected interrupt OUT");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type EcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // Header + Union + Ethernet Networking (matching kernel f_ecm.c's ecm_*_descriptor)
    data_type descriptor;

    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    // Union: bMasterInterface is this interface number, and bSlaveInterface0 is the data interface number
    //(associated by set_data_handler during assembly, obtained from the data handler's get_interface();
    // a composite device's communication interface does not start at interface 0, so hardcoding prevents the host driver finding the data interface).
    // Fall back to the single-device form when unassociated (COMM=0 -> DATA=1)
    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           get_interface().interface_number, // bMasterInterface: this interface
                           data_handler_ ? static_cast<std::uint8_t>(data_handler_->get_interface().interface_number) : std::uint8_t{1}}
            .append_to(descriptor);

    CdcEthernetFunctionalDesc{0x0D, CS_INTERFACE, 0x0F /* EthernetNetworking */,
                              mac_string_index_,
                              0, // bmEthernetStatistics: no statistics collected
                              1514, // wMaxSegmentSize: match kernel ETH_FRAME_LEN
                              0, // wNumberMCFilters
                              0} // bNumberPowerFilters
            .append_to(descriptor);

    return descriptor;
}

void EcmCommunicationInterfaceHandler::on_set_packet_filter(std::uint16_t filter) {
    // Default empty implementation; subclasses may override
}

void EcmCommunicationInterfaceHandler::send_network_connection(bool up) {
    // Put state in wValue (0/1), with no data bytes, matching kernel ecm_do_notify
    notification_channel.push(make_cdc_notification(EcmNotification::NetworkConnection, up ? 1 : 0,
                                                    interface_number_, nullptr, 0));
}

void EcmCommunicationInterfaceHandler::send_speed_change(std::uint32_t up_speed, std::uint32_t down_speed) {
    // 8 bytes of data = 4 bytes each for up/down, little-endian, matching kernel ecm_do_notify
    std::uint8_t speed_data[8];
    speed_data[0] = static_cast<std::uint8_t>(up_speed & 0xFF);
    speed_data[1] = static_cast<std::uint8_t>((up_speed >> 8) & 0xFF);
    speed_data[2] = static_cast<std::uint8_t>((up_speed >> 16) & 0xFF);
    speed_data[3] = static_cast<std::uint8_t>((up_speed >> 24) & 0xFF);
    speed_data[4] = static_cast<std::uint8_t>(down_speed & 0xFF);
    speed_data[5] = static_cast<std::uint8_t>((down_speed >> 8) & 0xFF);
    speed_data[6] = static_cast<std::uint8_t>((down_speed >> 16) & 0xFF);
    speed_data[7] = static_cast<std::uint8_t>((down_speed >> 24) & 0xFF);
    notification_channel.push(make_cdc_notification(EcmNotification::ConnectionSpeedChange, 0, interface_number_,
                                                    speed_data, sizeof(speed_data)));
}

void EcmCommunicationInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    notification_channel.on_new_connection(&current_session);
}

void EcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically), then let the parent clear session
    notification_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void EcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's CdcAcm
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== EcmDataInterfaceHandler ====================

EcmDataInterfaceHandler::EcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                 NetworkBackend *backend) :
    VirtualInterfaceHandler(handle_interface, string_pool), backend_(backend) {
    if (backend_) {
        // Inject the frame-send callback: frames produced by the backend are pushed to the host through the channel (thread-safe; callable from any thread)
        backend_->set_send_to_host([this](const std::uint8_t *data, std::size_t size) {
            in_channel.push(data_type(data, data + size));
        });
    }
}

UsbInterface EcmDataInterfaceHandler::make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
    // alt0 has no endpoints / alt1 has two bulk endpoints: endpoints' outer index is the alternate setting number
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {{},
                          {UsbEndpoint{.address = in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0},
                           UsbEndpoint{.address = out_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0}}},
    };
    return i;
}

void EcmDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags,
                                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                   std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN: the host requests a frame. Handled within the channel: reply immediately if a frame is buffered (one message = one frame;
        // if shorter than the request, send a short packet; host usbnet delimits frames by short/full-packet boundaries); otherwise suspend and wait
        SPDLOG_TRACE("[ECM] IN req seq={} len={} buf={}", seqnum, transfer_buffer_length, in_channel.size());
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT: the host sends a frame. If backend exists, give it directly to the network side for consumption and reply immediately;
        // otherwise let the subclass consume immediately (true=handled); if neither works, suspend in the channel (NAK
        // backpressure), and the subclass later takes data with take_frame() and replies
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        SPDLOG_TRACE("[ECM] OUT frame size={}", received_size);
        if (backend_) {
            backend_->send_frame(trx->data.data(), trx->data.size());
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else if (on_frame_received(std::move(trx->data))) {
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void EcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // The data interface has no class-specific control requests
    SPDLOG_WARN("ECM data interface received unexpected control request");
    // transfer is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type EcmDataInterfaceHandler::get_class_specific_descriptor() {
    // The data interface has no class-specific descriptors
    return {};
}

bool EcmDataInterfaceHandler::on_frame_received(data_type &&frame) {
    // Default behavior: always consume and reply immediately (subclasses without overrides retain always-receive semantics)
    return true;
}

std::size_t EcmDataInterfaceHandler::send_frame(const std::uint8_t *data, std::size_t size) {
    // Nonblocking push (the channel discards and returns 0 when disconnected); internally responds to pending IN requests
    if (data == nullptr || size == 0) {
        return 0;
    }
    SPDLOG_TRACE("[ECM] push frame size={} buf_before={}", size, in_channel.size());
    in_channel.push(data_type(data, data + size));
    SPDLOG_TRACE("[ECM] pushed, buf_after={}", in_channel.size());
    return size;
}

std::size_t EcmDataInterfaceHandler::send_frame(const data_type &frame) {
    return send_frame(frame.data(), frame.size());
}

std::optional<OutEndpointChannel::Pending> EcmDataInterfaceHandler::take_frame(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> EcmDataInterfaceHandler::try_take_frame() {
    return out_channel.try_take();
}

void EcmDataInterfaceHandler::set_tx_max_pending(std::size_t max_pending) {
    // The channel's internal lock protects the buffer limit
    in_channel.set_max_pending_messages(max_pending);
}

void EcmDataInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void EcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically; wake blocked
    // takers to return as disconnected), clear backend buffers to prevent old data remaining, then let the parent clear session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    if (backend_) {
        backend_->reset();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void EcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // Pending requests are distributed across the IN/OUT channels; try cancelling each in turn
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's CdcAcm
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
} // namespace usbipdcpp
