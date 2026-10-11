#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

void VirtualInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                   TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Default bulk-transfer implementation for virtual interface at endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                        std::error_code &ec) {
    SPDLOG_TRACE("Default interrupt-transfer implementation for virtual interface at endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                          int num_iso_packets, std::error_code &ec) {
    SPDLOG_TRACE("Default isochronous-transfer implementation for virtual interface at endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Default non-standard control-transfer implementation for virtual interface at endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Default non-standard control-transfer implementation with endpoint address {:04x} as recipient", ep.address);
    // TransferHandle is released automatically on destruction
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// Default standard-request callbacks are inline in the header (all are simple; the header is the documentation); cpp retains only the data plane
