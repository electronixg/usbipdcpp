#pragma once

#include "usbipdcpp/constant.h"

#include <cstdint>

namespace usbipdcpp {

/// Returns the maximum packet size (bytes) allowed by the bus according to the USB speed and endpoint type
constexpr std::uint16_t max_packet_size_limit(UsbSpeed speed, EndpointAttributes type) {
    switch (speed) {
        case UsbSpeed::Low:
            if (type == EndpointAttributes::Interrupt || type == EndpointAttributes::Control)
                return 8;
            return 0; // low speed does not support Bulk/Isochronous
        case UsbSpeed::Full:
            switch (type) {
                case EndpointAttributes::Control:
                    return 64;
                case EndpointAttributes::Interrupt:
                    return 64;
                case EndpointAttributes::Bulk:
                    return 64;
                case EndpointAttributes::Isochronous:
                    return 1023;
            }
        case UsbSpeed::High:
            switch (type) {
                case EndpointAttributes::Control:
                    return 64;
                case EndpointAttributes::Interrupt:
                    return 1024;
                case EndpointAttributes::Bulk:
                    return 512;
                case EndpointAttributes::Isochronous:
                    return 1024;
            }
        case UsbSpeed::Super:
            [[fallthrough]];
        case UsbSpeed::SuperPlus:
            switch (type) {
                case EndpointAttributes::Control:
                    return 512;
                case EndpointAttributes::Isochronous:
                    [[fallthrough]];
                case EndpointAttributes::Interrupt:
                    [[fallthrough]];
                case EndpointAttributes::Bulk:
                    return 1024;
            }
        default:
            return 0;
    }
}

/// EP0 Control transfer maximum packet size
constexpr std::uint16_t ep0_max_packet_size(UsbSpeed speed) {
    return max_packet_size_limit(speed, EndpointAttributes::Control);
}

struct UsbEndpoint {
    enum Direction { In, Out };

    std::uint8_t address = 0;
    std::uint8_t attributes = 0;
    std::uint16_t max_packet_size = 0;
    std::uint8_t interval = 0;


    [[nodiscard]] Direction direction() const {
        // A high bit of 1 indicates input
        if ((address & 0b10000000) != 0) {
            return Direction::In;
        }
        else {
            return Direction::Out;
        }
    }

    [[nodiscard]] bool is_in() const {
        return direction() == Direction::In;
    }

    [[nodiscard]] bool is_ep0() const {
        return (address & 0x7F) == 0;
    }

    static UsbEndpoint get_ep0_in(std::uint16_t max_packet_size) {
        return {.address = 0x80, // IN endpoint, the direction bit is 1
                .attributes = static_cast<std::uint8_t>(EndpointAttributes::Control),
                .max_packet_size = max_packet_size,
                .interval = 0};
    }

    static UsbEndpoint get_ep0_in(UsbSpeed speed) {
        return get_ep0_in(ep0_max_packet_size(speed));
    }

    static UsbEndpoint get_ep0_out(std::uint16_t max_packet_size) {
        return {.address = 0x00, // OUT endpoint, the direction bit is 0
                .attributes = static_cast<std::uint8_t>(EndpointAttributes::Control),
                .max_packet_size = max_packet_size,
                .interval = 0};
    }

    static UsbEndpoint get_ep0_out(UsbSpeed speed) {
        return get_ep0_out(ep0_max_packet_size(speed));
    }
};

} // namespace usbipdcpp
