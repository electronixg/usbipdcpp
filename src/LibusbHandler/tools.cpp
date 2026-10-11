#include "usbipdcpp/LibusbHandler/tools.h"

#include <spdlog/spdlog.h>

using namespace usbipdcpp;

UsbSpeed usbipdcpp::libusb_speed_to_usb_speed(int speed) {
    // libusb enumeration values differ from the kernel usb_device_speed enumeration (which has
    // USB_SPEED_WIRELESS in between), so do not cast directly; map individually:
    // LIBUSB_SPEED_SUPER=4 → USB_SPEED_SUPER=5,LIBUSB_SPEED_SUPER_PLUS=5 → 6
    // UsbSpeed enumeration values match the kernel enumeration (constant.h), allowing direct serialization to the protocol speed field
    switch (speed) {
        case LIBUSB_SPEED_LOW:
            return UsbSpeed::Low;
        case LIBUSB_SPEED_FULL:
            return UsbSpeed::Full;
        case LIBUSB_SPEED_HIGH:
            return UsbSpeed::High;
        case LIBUSB_SPEED_SUPER:
            return UsbSpeed::Super;
        case LIBUSB_SPEED_SUPER_PLUS:
            return UsbSpeed::SuperPlus;
        default:
            SPDLOG_DEBUG("unknown speed enum {}", speed);
            return UsbSpeed::Unknown;
    }
}
