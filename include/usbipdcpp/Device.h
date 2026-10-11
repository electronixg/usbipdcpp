#pragma once

#include <array>
#include <filesystem>
#include <variant>
#include <memory>

#include "usbipdcpp/Version.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/Interface.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/Export.h"


namespace usbipdcpp {
class Session;

class AbstDeviceHandler;

struct UsbIpIsoPacketDescriptor;

namespace UsbIpCommand {
    struct OpReqDevlist;
    struct OpReqImport;
    struct UsbIpCmdSubmit;
    struct UsbIpCmdUnlink;
    using AllCmdVariant = std::variant<OpReqDevlist, OpReqImport, UsbIpCmdSubmit, UsbIpCmdUnlink>;
}

struct USBIPDCPP_API UsbDevice {
    std::filesystem::path path{};
    std::string busid{};
    std::uint32_t bus_num;
    std::uint32_t dev_num;
    std::uint32_t speed;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    Version device_bcd{0, 0, 0};
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t configuration_value;
    std::uint8_t num_configurations;
    std::vector<UsbInterface> interfaces{};


    UsbEndpoint ep0_in;
    UsbEndpoint ep0_out;

    /**
     * @brief Device handler
     * must be set before calling Server::add_device. It is recommended to use with_handler function.
     * If handler is empty when called, it is undefined behavior.
     */
    std::shared_ptr<AbstDeviceHandler> handler;

    /**
     * @brief Assign interface_number to every interface in index order (0, 1, 2, ...)
     *
     * Suitable for virtual devices whose interfaces are numbered consecutively from 0 (the default case; call once after creating
     * a multi-interface mock device). Devices with gaps/custom numbering (such as a libusb backend populated from configuration descriptors)
     * do not need to call this function
     */
    void assign_interface_numbers() {
        for (std::size_t i = 0; i < interfaces.size(); i++) {
            interfaces[i].interface_number = static_cast<std::uint8_t>(i);
        }
    }

    /**
     * @brief Find N interfaces with consecutive interface_number values (first, first+1, ..., first+N-1)
     *
     * In a composite device, interfaces belonging to a function may be interleaved with other functions (nonconsecutive array indices); locating them by interface_number
     * is reliable (for example, UvcDeviceHelper finds the two adjacent VC/VS interfaces).
     * @tparam N The number of interfaces to be found
     * @param first_interface_number interface_number of the first interface
     * @return Pointers for found interfaces and nullptr for missing ones (N entries, with each index corresponding to a number offset)
     */
    template<std::size_t N>
    std::array<UsbInterface *, N> find_interfaces_by_number(std::uint8_t first_interface_number) {
        std::array<UsbInterface *, N> result{};
        for (auto &intf : interfaces) {
            auto num = intf.interface_number;
            if (num >= first_interface_number && num - first_interface_number < N) {
                result[num - first_interface_number] = &intf;
            }
        }
        return result;
    }

    /**
     * @brief Create and set handler
     * It is recommended to use this function to set the handler.
     * @tparam T handler type
     * @tparam args Parameters passed to the handler constructor
     * @return the handler created
     */
    template<typename T, typename... Args>
    std::shared_ptr<T> with_handler(Args &&... args) {
        auto new_handler = std::make_shared<T>(*this, std::forward<Args>(args)...);
        handler = std::static_pointer_cast<AbstDeviceHandler>(new_handler);
        return new_handler;
    }

    /**
     * @brief Create a basic virtual device: take the interface list (usually created by each interface handler's
     * make_interface, with handlers already bound) and frequently changed fields as parameters; give other fields default values;
     * EP0 The maximum packet size is automatically generated according to speed.
     *
     * parameters are arranged from high to low according to the frequency of change (there is no need to fill in the following parameters when changing the previous parameters).
     * After returning , you can still change any field, and then use with_handler to bind the device-level handler.
     * @param busid device busid (such as "1-1"), devlist/attach uses it to identify
     * @param vendor_id Manufacturer ID
     * @param product_id Product ID
     * @param interfaces device interface list
     * @param bus_num bus number (default 1)
     * @param dev_num device number (default 1)
     * @param device_class Device level class identifier (default 0 = defined at interface level; IAD composite device e.g.
     *        CDC requires 0x02)
     * @param path device path (for devlist display); default empty = "/usbipdcpp/<busid>"
     * @param speed device speed (default Full)
     * @param device_bcd device version number (BCD, default 1.0)
     * @param device_subclass / device_protocol device-level subclass/protocol (default 0)
     * @param configuration_value / num_configurations Configuration value and number of configurations (default 1)
     */
    static std::shared_ptr<UsbDevice> make(std::string busid, std::uint16_t vendor_id, std::uint16_t product_id,
                                           std::vector<UsbInterface> interfaces,
                                           std::uint32_t bus_num = 1, std::uint32_t dev_num = 1,
                                           std::uint8_t device_class = 0,
                                           std::filesystem::path path = {},
                                           UsbSpeed speed = UsbSpeed::Full,
                                           Version device_bcd = Version{0, 1, 0},
                                           std::uint8_t device_subclass = 0, std::uint8_t device_protocol = 0,
                                           std::uint8_t configuration_value = 1, std::uint8_t num_configurations = 1) {
        auto device = std::make_shared<UsbDevice>();
        device->path = path.empty() ? std::filesystem::path("/usbipdcpp/" + busid) : path;
        device->busid = std::move(busid);
        device->bus_num = bus_num;
        device->dev_num = dev_num;
        device->speed = static_cast<std::uint32_t>(speed);
        device->vendor_id = vendor_id;
        device->product_id = product_id;
        device->device_bcd = device_bcd;
        device->device_class = device_class;
        device->device_subclass = device_subclass;
        device->device_protocol = device_protocol;
        device->configuration_value = configuration_value;
        device->num_configurations = num_configurations;
        device->interfaces = std::move(interfaces);
        device->ep0_in = UsbEndpoint::get_ep0_in(speed);
        device->ep0_out = UsbEndpoint::get_ep0_out(speed);
        return device;
    }

    static constexpr std::size_t bytes_without_interfaces_num = calculate_total_size_with_array<
        array_data_type<256>,
        array_data_type<32>,
        decltype(bus_num),
        decltype(dev_num),
        decltype(speed),
        decltype(vendor_id),
        decltype(product_id),
        std::uint16_t,
        decltype(device_class),
        decltype(device_subclass),
        decltype(device_protocol),
        decltype(configuration_value),
        decltype(num_configurations),
        std::uint8_t
    >();

    [[nodiscard]] std::vector<std::uint8_t> to_bytes_with_interfaces() const;
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes_without_interfaces() const;

    //Send interface information for devlist requests, but not for import requests
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    std::optional<std::pair<UsbEndpoint, std::optional<UsbInterface>>> find_ep(std::uint8_t ep);

    bool operator==(const UsbDevice &other) const {
        return path == other.path &&
               busid == other.busid &&
               bus_num == other.bus_num &&
               dev_num == other.dev_num &&
               speed == other.speed &&
               vendor_id == other.vendor_id &&
               product_id == other.product_id &&
               device_bcd == other.device_bcd &&
               device_class == other.device_class &&
               device_subclass == other.device_subclass &&
               device_protocol == other.device_protocol &&
               configuration_value == other.configuration_value &&
               num_configurations == other.num_configurations &&
               interfaces == other.interfaces;
    }
};

static_assert(Serializable<UsbDevice>);
}
