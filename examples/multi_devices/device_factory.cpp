#include "device_factory.h"
#include "simple_device.h"

#include <format>
#include <spdlog/spdlog.h>

#include "usbipdcpp/virtual_device/devices/RelativeMouseHandler.h"

std::shared_ptr<usbipdcpp::UsbDevice> DeviceFactory::create_simple_device(int index,
                                                                          usbipdcpp::StringPool &string_pool) {
    // The interface description of  is the same as that of RelativeMouseHandler (HID 03/00/00 + interrupt IN 8/10), reusing its factory-built interface
    std::vector<usbipdcpp::UsbInterface> interfaces = {
            usbipdcpp::RelativeMouseHandler::make_interface(0x81),
    };

    // creates device (dev_num and path are customized with index)
    auto device = usbipdcpp::UsbDevice::make(generate_busid(index), generate_vendor_id(index),
                                             generate_product_id(index), std::move(interfaces),
                                             1, static_cast<std::uint32_t>(index), 0, generate_path(index));

    // Sets the example's own handler for the interface (overrides descriptor template semantics)
    device->interfaces[0].with_handler<SimpleHidInterfaceHandler>(string_pool);

    // Set the device handler
    auto device_handler = device->with_handler<SimpleDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    SPDLOG_INFO("Created device {}: VID={:04x} PID={:04x} busid={}", index, generate_vendor_id(index),
                generate_product_id(index), generate_busid(index));

    return device;
}

std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> DeviceFactory::create_devices(int count,
                                                                                 usbipdcpp::StringPool &string_pool) {
    std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> devices;
    devices.reserve(count);

    for (int i = 1; i <= count; ++i) {
        devices.push_back(create_simple_device(i, string_pool));
    }

    SPDLOG_INFO("Created {} virtual devices", count);
    return devices;
}

std::string DeviceFactory::generate_busid(int index) {
    return std::format("1-{}", index);
}

std::string DeviceFactory::generate_path(int index) {
    return std::format("/usbipdcpp/simple_device_{}", index);
}

std::uint16_t DeviceFactory::generate_vendor_id(int index) {
    // uses basic VID + index to ensure that each device has a different VID
    return static_cast<std::uint16_t>(0x1234 + index - 1);
}

std::uint16_t DeviceFactory::generate_product_id(int index) {
    // uses basic PID + index to ensure that each device has a different PID
    return static_cast<std::uint16_t>(0x5678 + index - 1);
}
