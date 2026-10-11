#pragma once

#include <string>
#include <format>

#include <libusb-1.0/libusb.h>
#include "usbipdcpp/constant.h"
#include "usbipdcpp/Export.h"


namespace usbipdcpp {
inline std::string get_device_busid(libusb_device *device) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(device, ports, 8);
    auto busid = std::to_string(libusb_get_bus_number(device));
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            busid += (i == 0 ? "-" : ".");
            busid += std::to_string(ports[i]);
        }
    } else {
        // topology information is not available (such as Android wrap_sys_device), use bus-addr:port format.
        // addr is unique on the bus, and the colon will not appear in the normal topology path and will not conflict with any device.
        busid += "-" + std::to_string(libusb_get_device_address(device))
              + ":" + std::to_string(libusb_get_port_number(device));
    }
    return busid;
}

USBIPDCPP_API UsbSpeed libusb_speed_to_usb_speed(int speed);
}
