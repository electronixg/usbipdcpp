#include "usbipdcpp/Interface.h"

std::vector<std::uint8_t> usbipdcpp::UsbInterface::to_bytes() const {
    // The OP_REP_DEVLIST interface structure in the protocol is exactly 4 bytes (class/subclass/protocol/padding),
    // without endpoint descriptors: struct usbip_usb_interface in the kernel's usbip_common.h contains only
    // these four fields, and usbipd's send_reply_devlist sends only that structure. After import, endpoints
    // are obtained by the client itself through control transfers (GET_DESCRIPTOR), independently of the device list
    std::vector<std::uint8_t> result(4, 0);
    result[0] = interface_class;
    result[1] = interface_subclass;
    result[2] = interface_protocol;
    return result;
}

void usbipdcpp::UsbInterface::from_socket(asio::ip::tcp::socket &sock) {
    interface_class = read_u8(sock);
    interface_subclass = read_u8(sock);
    interface_protocol = read_u8(sock);
    [[maybe_unused]] auto padding = read_u8(sock);
}
