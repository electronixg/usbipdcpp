#pragma once

#include <memory>
#include <string>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"

/**
 * @brief device factory class, used to create virtual USB devices
 */
class DeviceFactory {
public:
    /**
     * @brief Create a simple virtual HID device
     * @param index device index (1-10)
     * @param string_pool string pool reference
     * @return created device shared pointer
     */
    static std::shared_ptr<usbipdcpp::UsbDevice> create_simple_device(int index, usbipdcpp::StringPool &string_pool);

    /**
     * @brief Create multiple virtual devices
     * @param count number of devices
     * @param string_pool string pool reference
     * @return device list
     */
    static std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> create_devices(int count,
                                                                             usbipdcpp::StringPool &string_pool);

private:
    /**
     * @brief Generate device busid
     * @param index device index
     * @return busid string, format such as "1-1", "1-2" etc.
     */
    static std::string generate_busid(int index);

    /**
     * @brief Generate device path
     * @param index device index
     * @return device path
     */
    static std::string generate_path(int index);

    /**
     * @brief Generate vendor ID
     * @param index device index
     * @return vendor ID
     */
    static std::uint16_t generate_vendor_id(int index);

    /**
     * @brief Generate product ID
     * @param index device index
     * @return product ID
     */
    static std::uint16_t generate_product_id(int index);
};
