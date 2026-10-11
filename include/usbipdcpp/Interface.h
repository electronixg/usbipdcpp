#pragma once

#include <cstdint>
#include <optional>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/Export.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {
class VirtualInterfaceHandler;

struct USBIPDCPP_API UsbInterface {
    std::uint8_t interface_class;
    std::uint8_t interface_subclass;
    std::uint8_t interface_protocol;

    /// USB specification interface number (bInterfaceNumber), the wIndex of SET_INTERFACE is matched with it.
    /// Default 0 is consistent with the array index (virtual device interfaces are numbered consecutively from 0);
    /// The libusb backend fills this from the configuration descriptor when binding a physical device (for devices with numbering gaps, index ≠ interface number)
    std::uint8_t interface_number = 0;

    /// IAD for the function (interface group) to which this interface belongs, set on the function's first interface: during configuration descriptor
    /// generation, insert it before the corresponding interface descriptor and fill bFirstInterface from this interface's
    /// interface_number. A nonserialized field (the devlist protocol sends only 4 bytes of
    /// class/subclass/protocol information), excluded from to_bytes/from_socket
    std::optional<IadDesc> interface_association_descriptor;

    std::uint8_t current_altsetting = 0;

    /// The endpoint list of all alternate settings, [altsetting] = the endpoint of this alt
    std::vector<std::vector<UsbEndpoint>> endpoints;

    [[nodiscard]] const std::vector<UsbEndpoint> &current_endpoints() const {
        // Interfaces without endpoints (such as pure control interfaces) return an empty list to prevent endpoints[0] from going out of bounds
        static const std::vector<UsbEndpoint> no_endpoints;
        return endpoints.empty()
                       ? no_endpoints
                       : endpoints[current_altsetting < endpoints.size() ? current_altsetting : 0];
    }
    std::vector<UsbEndpoint> &current_endpoints() {
        static std::vector<UsbEndpoint> no_endpoints;
        return endpoints.empty()
                       ? no_endpoints
                       : endpoints[current_altsetting < endpoints.size() ? current_altsetting : 0];
    }

    std::shared_ptr<VirtualInterfaceHandler> handler;

    /**
     * @brief Bind an interface handler (the handler constructor retains a reference to this interface)
     * @note Only calls to UsbInterface whose address will not change are allowed: handler saves the reference to *this
     *       reference, once the interface object is copied/moved/container expanded, the reference will be dangling. The correct usage is
     *       is called after the device is created (the interface has been entered into device->interfaces and the address is stable)
     *       device->interfaces[i].with_handler<T>(...); make_interface and other factories
     *       function internal calls are prohibited (see VirtualInterfaceHandler class documentation)
     */
    template<typename T, typename... Args>
    std::shared_ptr<T> with_handler(Args &&...args) {
        auto new_handler = std::make_shared<T>(*this, std::forward<Args>(args)...);
        handler = std::dynamic_pointer_cast<VirtualInterfaceHandler>(new_handler);
        return new_handler;
    }

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    bool operator==(const UsbInterface &other) const {
        return interface_class == other.interface_class && interface_subclass == other.interface_subclass &&
               interface_protocol == other.interface_protocol;
    };
};

static_assert(Serializable<UsbInterface>);
} // namespace usbipdcpp
