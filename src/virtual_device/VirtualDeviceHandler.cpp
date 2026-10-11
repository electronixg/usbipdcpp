// [CTRL] and other debug logs are removed at compile time by default (TRACE level); define
// USBIPDCPP_STRACE to retain them when investigating client initialization
#ifndef USBIPDCPP_STRACE
    #undef SPDLOG_ACTIVE_LEVEL
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;

void VirtualDeviceHandler::receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                                       std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {
    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    dispatch_urb(cmd, seqnum, ep, interface, transfer_flags, transfer_buffer_length, setup_packet, ec);
}

void VirtualDeviceHandler::dispatch_urb(const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum,
                                        const UsbEndpoint &ep, std::optional<UsbInterface> &interface,
                                        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                        const SetupPacket &setup_packet, usbipdcpp::error_code &ec) {
    // Control transfers are less common; Bulk/Interrupt are more common
    // Take only bmAttributes bit0-1 for transfer type; bit2-3 are ISO sync type and bit4-5 are usage type
    const auto xfer_type = ep.attributes & 0x03;
    if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Control)) [[unlikely]] {
        SPDLOG_TRACE("Control transfer: type={:02x} req={:02x} val={:04x} idx={:04x} len={}", setup_packet.request_type,
                    setup_packet.request, setup_packet.value, setup_packet.index, setup_packet.length);
        handle_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(cmd.transfer),
                           ec);
    }
    else if (interface.has_value()) [[likely]] {
        auto &intf = interface.value();
        // Bulk and Interrupt are most common
        if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) [[likely]] {
            SPDLOG_TRACE("Bulk transfer ep={:02x} len={}", ep.address, transfer_buffer_length);
            handle_bulk_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer), ec);
        }
        else if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_TRACE("Interrupt transfer ep={:02x} len={}", ep.address, transfer_buffer_length);
            handle_interrupt_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer),
                                      ec);
        }
        else if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            SPDLOG_TRACE("Isochronous transfer ep={:02x} len={} iso_packets={}", ep.address, transfer_buffer_length,
                        cmd.number_of_packets);
            int num_iso = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                  ? static_cast<int>(cmd.number_of_packets)
                                  : 0;
            handle_isochronous_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length,
                                        std::move(cmd.transfer), num_iso, ec);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("Unknown transfer type for endpoint {:02x}: {}", ep.address, ep.attributes);
            ec = make_error_code(ErrorType::INVALID_ARG);
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("Target interface does not exist for a non-control transfer");
        ec = make_error_code(ErrorType::INTERNAL_ERROR);
    }
}

void VirtualDeviceHandler::setup_interface_handlers() {
    // Register each interface's TransferOperator in the device-level routing table; dispatch by endpoint
    auto *device_op = static_cast<VirtualDeviceTransferOperator *>(get_transfer_operator());

    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            auto *virtual_handler = intf.handler.get();
            if (virtual_handler) {
                virtual_handler->set_device_handler(this);
                // Register the interface-level TransferOperator for all endpoints of all alternate settings of this interface
                auto *if_op = virtual_handler->get_transfer_operator();
                for (auto &alt_eps: intf.endpoints) {
                    for (auto &ep: alt_eps) {
                        device_op->register_endpoint_operator(ep.address, if_op);
                    }
                }
                virtual_handler->on_setup_interface_handlers();
            }
        }
    }
}

void VirtualDeviceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    AbstDeviceHandler::on_new_connection(current_session, ec);
    // Start the device-level transfer scheduler first: after ISO URBs are queued, it delays responses at frame cadence
    // (matching vudc v_start_timer). Only devices with the scheduler enabled start a thread
    if (use_transfer_scheduler) {
        transfer_scheduler.start();
    }
    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            intf.handler->on_new_connection(current_session, ec);
            if (ec) {
                // Subinterface connection establishment failed: first roll back interfaces already connected successfully (they may have started
                // sender threads; without disconnection notification these remain running and hang on join during destruction),
                // then revoke the session pointer registered by the base class, preventing a remaining pointer to a Session about to be
                // destroyed (use-after-free; see AbstDeviceHandler::register_session comment)
                error_code rollback_ec;
                for (auto &done: handle_device.interfaces) {
                    if (&done == &intf)
                        break;
                    if (done.handler)
                        done.handler->on_disconnection(rollback_ec);
                }
                // Rollback: stop the scheduler thread that was started
                if (use_transfer_scheduler) {
                    transfer_scheduler.stop();
                }
                remove_responder();
                break;
            }
        }
    }
}

void VirtualDeviceHandler::on_disconnection(error_code &ec) {
    // Stop the device-level transfer scheduler first (discard unfinished URBs, stop thread): the scheduler thread may be calling
    // responder->submit_ret_submit, so join must finish before the base class clears the session pointer
    if (use_transfer_scheduler) {
        transfer_scheduler.stop();
    }
    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            // Use an independent error code for each handler: an existing input error (e.g. EOF disconnection) must not contaminate
            // the check. Do not stop on error: interface cleanup is independent (e.g. if AC fails, AS must still
            // finalize WAV headers), so one failure must not affect other interfaces' cleanup
            usbipdcpp::error_code handler_ec;
            intf.handler->on_disconnection(handler_ec);
            if (handler_ec) {
                SPDLOG_WARN("Interface handler on_disconnection error: {}", handler_ec.message());
            }
        }
    }
    AbstDeviceHandler::on_disconnection(ec);
}

void VirtualDeviceHandler::change_device_ep0_max_size_by_speed() {
    auto speed = static_cast<UsbSpeed>(handle_device.speed);
    switch (speed) {
        case UsbSpeed::Unknown: {
            SPDLOG_WARN("Unknown device speed");
            [[fallthrough]];
        }
        case UsbSpeed::Low: {
            handle_device.ep0_in.max_packet_size = 8;
            handle_device.ep0_out.max_packet_size = 8;
            break;
        }
        case UsbSpeed::Full:
        case UsbSpeed::High:
        case UsbSpeed::Wireless:
        // In the following two standards, you start out in high speed mode,
        // so you fill in 64, and then when you switch to super fast it changes to 512,
        // so you fill in 9 here
        case UsbSpeed::Super:
        case UsbSpeed::SuperPlus: {
            handle_device.ep0_in.max_packet_size = 64;
            handle_device.ep0_out.max_packet_size = 64;
            break;
        }
        default: {
            SPDLOG_WARN("invalid speed value");
            break;
        }
    }
}

void VirtualDeviceHandler::handle_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                                              TransferHandle transfer, std::error_code &ec) {
    // Get the GenericTransfer pointer
    auto *trx = GenericTransfer::from_handle(transfer.get());

    // Debug: print all control requests (including standard requests) to investigate where Windows client initialization stops
    SPDLOG_TRACE("[CTRL] seq={} bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={} {}",
                seqnum, setup_packet.calc_request_type(), setup_packet.request, setup_packet.value,
                setup_packet.index, transfer_buffer_length, setup_packet.is_out() ? "OUT" : "IN");

    auto recipient = static_cast<RequestRecipient>(setup_packet.calc_recipient());
    // All standard requests are handled here
    //  Most control requests are standard requests
    if (setup_packet.calc_request_type() == static_cast<std::uint8_t>(RequestType::Standard)) [[likely]] {
        auto status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        switch (recipient) {
            case RequestRecipient::Device: {
                SPDLOG_TRACE("Send to device");
                auto std_request = static_cast<StandardRequest>(setup_packet.calc_standard_request());

                if (setup_packet.is_out()) {
                    switch (std_request) {
                        case StandardRequest::ClearFeature: {
                            SPDLOG_TRACE("Device ClearFeature");
                            request_clear_feature(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetAddress: {
                            SPDLOG_TRACE("Device SetAddress");
                            request_set_address(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetConfiguration: {
                            SPDLOG_TRACE("Device SetConfiguration");
                            request_set_configuration(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetDescriptor: {
                            SPDLOG_TRACE("Device SetDescriptor");
                            // Get OUT data from transfer_handle
                            data_type out_data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
                            request_set_descriptor(setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                                   setup_packet.index, setup_packet.length, out_data, &status);
                            break;
                        }
                        case StandardRequest::SetFeature: {
                            SPDLOG_TRACE("Device SetFeature");
                            request_set_feature(setup_packet.value, &status);
                            break;
                        }
                        default: {
                            SPDLOG_WARN("Device Unhandled StandardRequest {}", static_cast<int>(std_request));
                            status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                        }
                    }
                    // transfer is released automatically on destruction
                    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            seqnum, status, 0));
                }
                else {
                    data_type result{};
                    switch (std_request) {
                        case StandardRequest::GetConfiguration: {
                            SPDLOG_TRACE("Device GetConfiguration");
                            auto ret = request_get_configuration(&status);
                            // Control-transfer data stages are uniformly little-endian (GetConfiguration returns 1 byte,
                            // so endianness makes no difference; consistent with GetStatus and others)
                            vector_append_to_le(result, ret);
                            break;
                        }
                        case StandardRequest::GetDescriptor: {
                            SPDLOG_TRACE("Device GetDescriptor");
                            result = request_get_descriptor(setup_packet.value >> 8, setup_packet.value,
                                                            setup_packet.length, &status);
                            if (setup_packet.length < result.size()) {
                                result.resize(setup_packet.length);
                            }
                            break;
                        }
                        case StandardRequest::GetStatus: {
                            SPDLOG_TRACE("Device GetStatus");
                            auto gotten_status = request_get_status(&status);
                            // Multibyte fields in USB control-transfer data stages are little-endian (GetStatus returns 2 bytes of
                            // device status, low byte first); do not use network byte order
                            vector_append_to_le(result, gotten_status);
                            break;
                        }
                        default: {
                            SPDLOG_WARN("Device Unhandled StandardRequest {}", static_cast<int>(std_request));
                            // Return EPIPE for unhandled requests; do not leave default 0 (StatusOK) and report success
                            status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                        }
                    }
                    // Write data into transfer_handle
                    trx->data = std::move(result);
                    trx->actual_length = trx->data.size();
                    trx->data_offset = 0; // Virtual devices have no setup-packet offset

                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                    seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                    std::move(transfer)));
                }
                break;
            }
            case RequestRecipient::Interface: {
                SPDLOG_TRACE("Send to interface");
                auto intf_idx = setup_packet.index;
                if (intf_idx >= handle_device.interfaces.size()) {
                    SPDLOG_WARN("Interface number {} out of bounds ({} interfaces total); returning EPIPE", intf_idx, handle_device.interfaces.size());
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                auto handler = handle_device.interfaces[intf_idx].handler;
                if (handler) {
                    StandardRequest std_request = static_cast<StandardRequest>(setup_packet.calc_standard_request());
                    if (setup_packet.is_out()) {
                        switch (std_request) {
                            case StandardRequest::ClearFeature: {
                                SPDLOG_TRACE("Interface request_clear_feature");
                                handler->request_clear_feature(setup_packet.value, &status);
                                break;
                            }
                            case StandardRequest::SetFeature: {
                                SPDLOG_TRACE("Interface request_set_feature");
                                handler->request_set_feature(setup_packet.value, &status);
                                break;
                            }
                            case StandardRequest::SetInterface: {
                                SPDLOG_DEBUG("SET_INTERFACE: intf={} alt={}", intf_idx, setup_packet.value);
                                handler->request_set_interface(setup_packet.value, &status);
                                if (status == 0 &&
                                    setup_packet.value < handle_device.interfaces[intf_idx].endpoints.size())
                                    handle_device.interfaces[intf_idx].current_altsetting =
                                            static_cast<std::uint8_t>(setup_packet.value);
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Interface Unhandled StandardRequest {}", static_cast<int>(std_request));
                                // Return EPIPE for unhandled requests; do not leave default 0 (StatusOK) and report success
                                status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                            }
                        }
                        // transfer is released automatically on destruction
                        responder->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                        seqnum, status,
                                        0 // Control-transfer OUT command, no data stage
                                        ));
                    }
                    else {
                        data_type result{};
                        switch (std_request) {
                            case StandardRequest::GetInterface: {
                                SPDLOG_TRACE("Interface request_get_interface");
                                auto ret = this->request_get_interface(setup_packet.index, &status);
                                // Control-transfer data stages are uniformly little-endian (GetInterface returns 1 byte,
                                // so endianness makes no difference; consistent with GetStatus and others)
                                vector_append_to_le(result, ret);
                                break;
                            }
                            case StandardRequest::GetStatus: {
                                SPDLOG_TRACE("Interface request_get_status");
                                auto ret = handler->request_get_status(&status);
                                // Multibyte fields in USB control-transfer data stages are little-endian; do not use network byte order
                                vector_append_to_le(result, ret);
                                break;
                            }
                            case StandardRequest::GetDescriptor: {
                                SPDLOG_TRACE("Interface request_get_descriptor");
                                result = handler->request_get_descriptor(setup_packet.value >> 8,
                                                                         setup_packet.value & 0x00FF,
                                                                         setup_packet.length, &status);
                                if (setup_packet.length < result.size()) {
                                    result.resize(setup_packet.length);
                                }
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Interface Unhandled StandardRequest {}", static_cast<int>(std_request));
                                // Return EPIPE for unhandled requests; do not leave default 0 (StatusOK) and report success
                                status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                            }
                        }
                        // Write data into transfer_handle
                        trx->data = std::move(result);
                        trx->actual_length = trx->data.size();
                        trx->data_offset = 0;

                        responder->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                        seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                        std::move(transfer)));
                    }
                }
                else {
                    SPDLOG_ERROR("No handler registered for interface; cannot handle messages sent to it");
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    // transfer is released automatically on destruction
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Endpoint: {
                SPDLOG_TRACE("Send to endpoint");
                auto find_ret = handle_device.find_ep(setup_packet.index);
                if (find_ret) [[likely]] {
                    // auto &target_ep = find_ret->first;
                    auto &intf = find_ret->second;
                    if (intf) [[likely]] {
                        auto handler = intf->handler;
                        if (handler) [[likely]] {
                            StandardRequest std_request =
                                    static_cast<StandardRequest>(setup_packet.calc_standard_request());
                            if (setup_packet.is_out()) {
                                switch (std_request) {
                                    case StandardRequest::ClearFeature: {
                                        SPDLOG_TRACE("Endpoint request_endpoint_clear_feature");
                                        handler->request_endpoint_clear_feature(setup_packet.value, setup_packet.index,
                                                                                &status);
                                        break;
                                    }
                                    case StandardRequest::SetFeature: {
                                        SPDLOG_TRACE("Endpoint request_endpoint_set_feature");
                                        handler->request_endpoint_set_feature(setup_packet.value, setup_packet.index,
                                                                              &status);
                                        break;
                                    }
                                    default: {
                                        SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}", setup_packet.index,
                                                    static_cast<int>(std_request));
                                        // Return EPIPE for unhandled requests; do not leave default 0 (StatusOK) and report success
                                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                                    }
                                }
                                // transfer is released automatically on destruction
                                responder->submit_ret_submit(
                                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                                seqnum, status,
                                                0 // Control-transfer OUT command, no data stage
                                                ));
                            }
                            else {
                                data_type result{};
                                switch (std_request) {
                                    case StandardRequest::GetStatus: {
                                        SPDLOG_TRACE("Endpoint request_endpoint_get_status");
                                        auto gotten_status =
                                                handler->request_endpoint_get_status(setup_packet.index, &status);
                                        // Multibyte fields in USB control-transfer data stages are little-endian; do not use network byte order
                                        vector_append_to_le(result, gotten_status);
                                        break;
                                    }
                                    case StandardRequest::SynchFrame: {
                                        SPDLOG_TRACE("Endpoint request_endpoint_sync_frame");
                                        handler->request_endpoint_sync_frame(setup_packet.index, &status);
                                        break;
                                    }
                                    default: {
                                        SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}", setup_packet.index,
                                                    static_cast<int>(std_request));
                                        // Return EPIPE for unhandled requests; do not leave default 0 (StatusOK) and report success
                                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                                    }
                                }
                                // Write data into transfer_handle
                                trx->data = std::move(result);
                                trx->actual_length = trx->data.size();
                                trx->data_offset = 0;

                                responder->submit_ret_submit(
                                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                                seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                                std::move(transfer)));
                            }
                        }
                        else {
                            SPDLOG_ERROR("No corresponding handler registered for the interface containing endpoint {:04x}", setup_packet.value);
                            ec = make_error_code(ErrorType::INVALID_ARG);
                            // transfer is released automatically on destruction
                            responder->submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                            return;
                        }
                    }
                    else {
                        SPDLOG_ERROR("No interface corresponds to endpoint {:04x}", setup_packet.value);
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        // transfer is released automatically on destruction
                        responder->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                        return;
                    }
                }
                break;
            }
            case RequestRecipient::Other: {
                SPDLOG_TRACE("Send to other");
                SPDLOG_WARN("Packets to other recipients are unimplemented");
                // transfer is released automatically on destruction
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                break;
            }
            default: {
                SPDLOG_WARN("Unknown recipient");
                // transfer is released automatically on destruction
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
        }
    }
    else {
        switch (recipient) {
            case RequestRecipient::Device: {
                SPDLOG_TRACE("Non-standard control-transfer packet sent to device");
                handle_non_standard_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                             setup_packet, std::move(transfer), ec);
                break;
            }
            case RequestRecipient::Interface: {
                SPDLOG_TRACE("Non-standard control-transfer packet sent to interface {}", setup_packet.index);
                auto intf_idx = setup_packet.index & 0xFF;
                if (intf_idx >= handle_device.interfaces.size()) {
                    SPDLOG_WARN("Interface number {} out of bounds ({} interfaces total); returning EPIPE", intf_idx, handle_device.interfaces.size());
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                auto handler = handle_device.interfaces[intf_idx].handler;
                if (handler) {
                    handler->handle_non_standard_request_type_control_urb(
                            seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(transfer), ec);
                }
                else {
                    SPDLOG_ERROR("No handler registered for interface; cannot handle messages sent to it");
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    // transfer is released automatically on destruction
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Endpoint: {
                SPDLOG_TRACE("Non-standard control-transfer packet sent to endpoint address {}", setup_packet.index);
                auto find_ret = handle_device.find_ep(setup_packet.index);
                if (find_ret) {
                    auto &intf = find_ret->second;
                    if (intf) [[likely]] {
                        auto handler = intf->handler;
                        if (handler) [[likely]] {
                            handler->handle_non_standard_request_type_control_urb_to_endpoint(
                                    seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                    std::move(transfer), ec);
                        }
                        else {
                            SPDLOG_ERROR("No corresponding handler registered for the interface containing endpoint {:04x}", setup_packet.index);
                            ec = make_error_code(ErrorType::INVALID_ARG);
                            responder->submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                            return;
                        }
                    }
                    else {
                        SPDLOG_ERROR("No interface corresponds to endpoint {:04x}", setup_packet.index);
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        responder->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                        return;
                    }
                }
                else {
                    SPDLOG_ERROR("Cannot find endpoint {:04x}", setup_packet.index);
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Other: {
                SPDLOG_WARN("Packets to other recipients are unimplemented");
                // transfer is released automatically on destruction
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                break;
            }
            default: {
                SPDLOG_WARN("Unknown recipient");
                // transfer is released automatically on destruction
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
        }
    }
}

void VirtualDeviceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, UsbInterface &interface,
                                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                TransferHandle transfer, std::error_code &ec) {
    if (interface.handler) [[likely]] {
        interface.handler->handle_bulk_transfer(seqnum, ep, transfer_flags, transfer_buffer_length, std::move(transfer),
                                                ec);
    }
    else {
        SPDLOG_ERROR("No handler registered for the interface containing endpoint {:04x}", ep.address);
        // transfer is released automatically on destruction
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     UsbInterface &interface, std::uint32_t transfer_flags,
                                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                     std::error_code &ec) {

    if (interface.handler) [[likely]] {
        interface.handler->handle_interrupt_transfer(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                     std::move(transfer), ec);
    }
    else {
        SPDLOG_ERROR("No handler registered for the interface containing endpoint {:04x}", ep.address);
        // transfer is released automatically on destruction
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       int num_iso_packets, std::error_code &ec) {
    if (interface.handler) [[likely]] {
        interface.handler->handle_isochronous_transfer(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                       std::move(transfer), num_iso_packets, ec);
    }
    else {
        SPDLOG_ERROR("No handler registered for the interface containing endpoint {:04x}", ep.address);
        // transfer is released automatically on destruction
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // Isochronous URBs reside in the scheduler queue; try cancelling there first (matching vudc_rx.c's
    // CMD_UNLINK: found -> respond RET_UNLINK(-ECONNRESET) without further RET_SUBMIT;
    // not found -> respond RET_UNLINK(0), indicating completed or nonexistent URB)
    if (use_transfer_scheduler && transfer_scheduler.cancel(unlink_seqnum)) {
        responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                cmd_seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET)));
        return;
    }
    for (auto &interface: handle_device.interfaces) {
        if (interface.handler) {
            interface.handler->handle_unlink_seqnum(unlink_seqnum, cmd_seqnum);
        }
    }
}

std::uint8_t VirtualDeviceHandler::request_get_configuration(std::uint32_t *p_status) {
    return handle_device.configuration_value;
}

data_type VirtualDeviceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                       std::uint16_t descriptor_length, std::uint32_t *p_status) {
    auto desc_type = static_cast<DescriptorType>(type);
    switch (desc_type) {
        case DescriptorType::Device: {
            SPDLOG_TRACE("Device get_device_descriptor");
            return get_device_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::Configuration: {
            SPDLOG_TRACE("Device get_configuration_descriptor");
            return get_configuration_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::DeviceQualifier: {
            SPDLOG_TRACE("Device get_device_qualifier_descriptor");
            return get_device_qualifier_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::BOS: {
            SPDLOG_TRACE("Device get_bos_descriptor");
            return get_bos_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::String: {
            SPDLOG_TRACE("Device get_string_descriptor");
            return get_string_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::OtherSpeedConfiguration: {
            SPDLOG_TRACE("Device get_other_speed_descriptor");
            return get_other_speed_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        default: {
            SPDLOG_DEBUG("Requesting non-standard descriptor {:08b}", type);
            return get_custom_descriptor(type, language_id, descriptor_length, p_status);
        }
    }
}

std::uint8_t VirtualDeviceHandler::request_get_interface(std::uint16_t intf, std::uint32_t *p_status) {
    auto handler = handle_device.interfaces[intf].handler;
    if (handler) {
        return handler->request_get_interface(p_status);
    }
    else {
        SPDLOG_ERROR("No handler registered for interface; cannot handle");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return -1;
    }
}

void VirtualDeviceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint16_t intf,
                                                 std::uint32_t *p_status) {
    auto &interface = handle_device.interfaces[intf];
    auto handler = interface.handler;
    if (handler) {
        handler->request_set_interface(alternate_setting, p_status);
        if (*p_status == 0 && alternate_setting < interface.endpoints.size()) {
            interface.current_altsetting = static_cast<std::uint8_t>(alternate_setting);
        }
    }
    else {
        SPDLOG_ERROR("No handler registered for interface; cannot handle");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return;
    }
}

data_type VirtualDeviceHandler::get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    data_type desc;
    DeviceDesc{0x12, static_cast<std::uint8_t>(DescriptorType::Device),
               usb_version, // bcdUSB
               handle_device.device_class, handle_device.device_subclass, handle_device.device_protocol,
               static_cast<std::uint8_t>(handle_device.ep0_in.max_packet_size), // bMaxPacketSize0
               handle_device.vendor_id, handle_device.product_id,
               static_cast<std::uint16_t>(handle_device.device_bcd.minor | (handle_device.device_bcd.major << 8)),
               string_manufacturer_value, string_product_value, string_serial_value, handle_device.num_configurations}
            .append_to(desc);

    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                   std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    // BOS header (5) + USB 2.0 Extension Device Capability (7) = 12 bytes
    data_type desc;
    BosHeaderDesc{0x05, static_cast<std::uint8_t>(DescriptorType::BOS),
                  0x0C, // wTotalLength = 12
                  0x01}
            .append_to(desc);
    BosUsb20ExtCapDesc{0x07,
                       0x10, // bDescriptorType: DEVICE CAPABILITY
                       0x02, // bDevCapabilityType: USB 2.0 EXTENSION
                       0x00} // bmAttributes: no LPM
            .append_to(desc);

    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_configuration_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                             std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    data_type desc;
    ConfigHeaderDesc{0x09, static_cast<std::uint8_t>(DescriptorType::Configuration),
                     0x00, // wTotalLength: fill after the concatenation loop completes
                     static_cast<std::uint8_t>(handle_device.interfaces.size()), // bNumInterfaces
                     handle_device.configuration_value, // bConfigurationValue
                     string_configuration_value, // iConfiguration
                     0x80, // bmAttributes Bus Powered
                     0xFA} // bMaxPower 500mA
            .append_to(desc);
    // IAD (Interface Association Descriptor): multi-interface functions (RNDIS/UVC, etc.) must have IAD to be recognized
    // as composite devices on Windows (matching each kernel gadget function's *_iad_descriptor).
    // Declared by the function's first interface's interface_association_descriptor and inserted before the owning
    // interface descriptor (the position required by USB 2.0 ECN); fill bFirstInterface with the owning interface
    // number (possibly unknown when declared, e.g. during the make_interface template stage)
    for (std::size_t i = 0; i < handle_device.interfaces.size(); i++) {
        auto &intf = handle_device.interfaces[i];
        if (intf.interface_association_descriptor) {
            auto iad = *intf.interface_association_descriptor;
            iad.bFirstInterface = intf.interface_number;
            iad.append_to(desc);
        }
        // Skip class-specific descriptors if no interface handler is registered (consistent with handle_control_urb's EPIPE handling)
        auto class_specific_descriptor =
                intf.handler ? intf.handler->get_class_specific_descriptor() : data_type{};

        auto num_alts = intf.endpoints.size();
        if (num_alts == 0)
            num_alts = 1; // Ensure at least alt 0 if uninitialized

        // Endpoint-free interfaces (e.g. control-only): use an empty alt_endpoints list to prevent endpoints[0] out-of-bounds access
        static const std::vector<UsbEndpoint> no_endpoints;
        for (std::size_t alt = 0; alt < num_alts; alt++) {
            auto &alt_endpoints = intf.endpoints.empty()
                                          ? no_endpoints
                                          : (alt < intf.endpoints.size() ? intf.endpoints[alt] : intf.endpoints[0]);
            data_type intf_desc;
            InterfaceDesc{0x09, static_cast<std::uint8_t>(DescriptorType::Interface),
                          intf.interface_number, // bInterfaceNumber: strictly follow interface_number
                          // semantics (manually filled by the assembler for multi-interface devices; see UsbInterface::interface_number)
                          static_cast<std::uint8_t>(alt), // bAlternateSetting
                          static_cast<std::uint8_t>(alt_endpoints.size()), // bNumEndpoints
                          intf.interface_class, // bInterfaceClass
                          intf.interface_subclass, // bInterfaceSubClass
                          intf.interface_protocol, // bInterfaceProtocol
                          (intf.handler ? intf.handler->get_string_interface_value() : std::uint8_t{0})} // iInterface
                    .append_to(intf_desc);
            // Put class-specific descriptors only in alt 0 by default (TinyUSB approach); alt>0 contains only endpoints.
            // Putting them in every alt would expand UVC config descriptors beyond 255 bytes, causing Windows truncation and parse failure.
            // UAC AS interface descriptors are small and alt 1 must have format descriptors; allow through put_class_specific_descriptor_in_all_alts
            bool all_alts = intf.handler && intf.handler->put_class_specific_descriptor_in_all_alts();
            if ((alt == 0 || all_alts) && !class_specific_descriptor.empty()) {
                intf_desc.insert(intf_desc.end(), class_specific_descriptor.begin(), class_specific_descriptor.end());
            }
            for (auto &endpoint: alt_endpoints) {
                EndpointDesc{0x07, static_cast<std::uint8_t>(DescriptorType::Endpoint),
                             endpoint.address, endpoint.attributes, endpoint.max_packet_size, endpoint.interval}
                        .append_to(intf_desc);

                // UVC 1.5 Table 3-12: VC interrupt endpoint requires class-specific endpoint descriptor
                if (intf.interface_class == CC_VIDEO && intf.interface_subclass == SC_VIDEOCONTROL &&
                    (endpoint.attributes & 0x03) == 0x03) {
                    VcInterruptEndpointDesc{0x05, CS_ENDPOINT, EP_INTERRUPT, endpoint.max_packet_size}.append_to(
                            intf_desc);
                }
                // UAC 1.0 section 4.6.1.2: AS ISO audio data endpoints require class-specific endpoint descriptors.
                // bmAttributes SamplingFreqControl is a prerequisite for Linux snd-usb-audio initiating
                // sample-rate SET_CUR negotiation; without it the driver starts streaming directly at the descriptor's default rate.
                // Keep standard endpoint descriptors at 7 bytes: although the audio specification requires 9 (audio10.pdf Table
                // 4-20), observed 9-byte descriptors cause Windows usbaudio startup failure with usbip-win;
                // 7 bytes work (long-term mock_audio verification; do not change)
                // UAC 1.0 section 4.6.1.2: AS ISO audio data endpoints require class-specific endpoint descriptors.
                // bmAttributes SamplingFreqControl is a prerequisite for Linux snd-usb-audio initiating
                // sample-rate SET_CUR negotiation; without it the driver starts streaming directly at the descriptor's default rate.
                // Keep standard endpoint descriptors at 7 bytes: although audio10.pdf Table 4-20 requires audio endpoints to have
                // 9 bytes (bRefresh/bSynchAddress), usbip-win2 uses the UDE framework,
                // and Udecx.sys parses configuration descriptors using the 7-byte USB_ENDPOINT_DESCRIPTOR;
                // 9 bytes misalign subsequent descriptors and immediately fail Windows enumeration (the server receives
                // no control requests). 7 bytes work in tests (long-term mock_audio verification); do not change
                if (intf.interface_class == CC_AUDIO && intf.interface_subclass == SC_AUDIOSTREAMING &&
                    (endpoint.attributes & 0x03) == 0x01) {
                    AsEpGeneralDesc{AS_EP_DESC_GENERAL_LEN,
                                    CS_ENDPOINT,
                                    AS_EP_DESC_GENERAL,
                                    AS_EP_ATTR_SAMPLING_FREQ,
                                    0x00, // bLockDelayUnits
                                    0x00} // wLockDelay
                            .append_to(intf_desc);
                }
            }
            desc.insert(desc.end(), intf_desc.begin(), intf_desc.end());
        }
    }
    desc[2] = static_cast<std::uint8_t>(desc.size());
    desc[3] = static_cast<std::uint8_t>(desc.size() >> 8);
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    // Try the virtual function first: allow subclasses to handle special string indices (e.g. Microsoft OS 0xEE)
    if (auto special = get_special_string_descriptor(language_id)) {
        if (descriptor_length < special->size())
            special->resize(descriptor_length);
        return *special;
    }

    if (language_id == 0) [[unlikely]] {
        // language ids - special case for obtaining the supported language ID list
        data_type desc = {4, static_cast<std::uint8_t>(DescriptorType::String), 0x09, 0x04};
        if (descriptor_length < desc.size()) {
            desc.resize(descriptor_length);
        }
        return desc;
    }
    else if (auto string_ret = string_pool.get_string(language_id)) [[likely]] {
        auto &string = string_ret.value();
        data_type desc = {
                static_cast<std::uint8_t>((string.size() + 1) * 2),
                static_cast<std::uint8_t>(DescriptorType::String),
        };
        for (auto &i: string) {
            desc.push_back(static_cast<std::uint8_t>(i));
            desc.push_back(static_cast<std::uint8_t>(i >> 8));
        }
        if (descriptor_length < desc.size()) {
            desc.resize(descriptor_length);
        }
        return desc;
    }
    else {
        SPDLOG_ERROR("Invalid string descriptor index: {}", language_id);
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }
}

data_type VirtualDeviceHandler::get_device_qualifier_descriptor(std::uint8_t language_id,
                                                                std::uint16_t descriptor_length,
                                                                std::uint32_t *p_status) {
    // USB 2.0 section 9.6.2: high-speed devices must return other-speed information
    // Return device descriptor information in full-speed mode (bMaxPacketSize0 and others match high speed)
    std::shared_lock lock(data_mutex);
    // Encode bcdUSB using usb_version's operator uint16_t() (BCD: major<<8 |
    // minor<<4 | patch), consistent with the device descriptor; do not split only minor/major,
    // omitting patch. Split into low and high bytes and put into the byte-array descriptor
    std::uint16_t bcd_usb = usb_version;
    data_type desc = {
            0x0A,                                                           // bLength
            static_cast<std::uint8_t>(DescriptorType::DeviceQualifier),     // bDescriptorType
            static_cast<std::uint8_t>(bcd_usb & 0x00FF),                    // bcdUSB low byte
            static_cast<std::uint8_t>(bcd_usb >> 8),                        // bcdUSB high byte
            handle_device.device_class,                                     // bDeviceClass
            handle_device.device_subclass,                                  // bDeviceSubClass
            handle_device.device_protocol,                                  // bDeviceProtocol
            static_cast<std::uint8_t>(handle_device.ep0_in.max_packet_size), // bMaxPacketSize0 (FS)
            handle_device.num_configurations,                               // bNumConfigurations
            0x00,                                                           // bReserved
    };
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

std::optional<data_type> VirtualDeviceHandler::get_special_string_descriptor(std::uint8_t string_index) {
    (void)string_index;
    return std::nullopt;
}

data_type VirtualDeviceHandler::get_custom_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                      std::uint16_t descriptor_length, std::uint32_t *p_status) {
    return {};
}
