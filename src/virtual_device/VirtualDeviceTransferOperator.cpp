// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "usbipdcpp/virtual_device/VirtualDeviceTransferOperator.h"

#include <spdlog/spdlog.h>
#include "usbipdcpp/constant.h"

using namespace usbipdcpp;

TransferOperator *VirtualDeviceTransferOperator::get_operator_for_ep(std::uint8_t ep) {
    auto it = ep_operators_.find(ep);
    return (it != ep_operators_.end()) ? it->second : &generic_op_;
}

void *VirtualDeviceTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                           const UsbIpHeaderBasic &header,
                                                           const SetupPacket &setup_packet) {
    // header.ep is a USB/IP wire-format endpoint number (without direction bit); add the direction bit according to direction
    // before looking up the registry (ep_operators_ keys are full endpoint addresses including direction; see
    // setup_interface_handlers register_endpoint_operator). Keep consistent with
    // UsbIpCmdSubmit::from_socket's real_ep reconstruction logic
    std::uint8_t real_ep = static_cast<std::uint8_t>(header.ep);
    if (header.direction == UsbIpDirection::In)
        real_ep |= 0x80;
    auto *leaf_op = get_operator_for_ep(real_ep);
    return leaf_op->alloc_transfer_handle(buffer_length, num_iso_packets, header, setup_packet);
}

void VirtualDeviceTransferOperator::free_transfer_handle(void *handle) {
    // leaf op is already stored in TransferHandle; the normal path never reaches here.
    // If reached, the caller is not using TransferHandle::get_operator() correctly.
    SPDLOG_ERROR("VDTO::free_transfer_handle handle={:p} should not be called", static_cast<const void *>(handle));
}

std::size_t VirtualDeviceTransferOperator::get_actual_length(void *handle) {
    return generic_op_.get_actual_length(handle);
}

bool VirtualDeviceTransferOperator::transfer_is_in(void *handle) {
    // Defensive path: normal flow binds TransferHandle to leaf op (see protocol.cpp
    // set_handle), so to_socket never receives this routing op
    return generic_op_.transfer_is_in(handle);
}

UsbIpIsoPacketDescriptor VirtualDeviceTransferOperator::get_iso_descriptor(void *handle, int index) {
    return generic_op_.get_iso_descriptor(handle, index);
}

void VirtualDeviceTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    generic_op_.set_iso_descriptor(handle, index, desc);
}

void VirtualDeviceTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.send_transfer_data(handle, sock, length, ec);
}

void VirtualDeviceTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.recv_transfer_data(handle, sock, length, ec);
}
