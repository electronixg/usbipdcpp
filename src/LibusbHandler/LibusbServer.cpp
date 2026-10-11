#include "usbipdcpp/LibusbHandler/LibusbServer.h"

#include <iostream>

#include <spdlog/spdlog.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"
#include "usbipdcpp/LibusbHandler/tools.h"

using namespace usbipdcpp;

namespace {
void log_device_state(Server &server) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &available = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    std::string avail_busids;
    for (auto &d: available) {
        if (!avail_busids.empty())
            avail_busids += ", ";
        avail_busids += d->busid;
    }
    std::string using_busids;
    for (auto &[busid, d]: using_devices) {
        if (!using_busids.empty())
            using_busids += ", ";
        using_busids += busid;
    }

    SPDLOG_INFO("Device status: {} available [{}] | {} in use [{}]", available.size(), avail_busids, using_devices.size(),
                using_busids);
}
} // namespace

LibusbServer::LibusbServer(const LibusbServerConfig& config) : config(config) {
}

std::pair<std::string, std::string> LibusbServer::get_device_names(libusb_device *device) {
    libusb_device_handle *handle = nullptr;
    libusb_device_descriptor desc{};
    int ret = libusb_get_device_descriptor(device, &desc);
    if (ret < 0) {
        return {"Unknown", "Unknown"};
    }

    // Try opening the device to get string descriptors
    char manufacturer[256] = {0};
    char product[256] = {0};

    if (libusb_open(device, &handle) == 0) {
        if (desc.iManufacturer) {
            libusb_get_string_descriptor_ascii(handle, desc.iManufacturer,
                                               reinterpret_cast<unsigned char *>(manufacturer), sizeof(manufacturer));
        }

        if (desc.iProduct) {
            libusb_get_string_descriptor_ascii(handle, desc.iProduct, reinterpret_cast<unsigned char *>(product),
                                               sizeof(product));
        }

        libusb_close(handle);
    }

    return {(manufacturer[0] ? manufacturer : "Unknown Manufacturer"), (product[0] ? product : "Unknown Product")};
}

void LibusbServer::print_device(libusb_device *dev, bool skip_name) {
    libusb_device_descriptor desc{};
    // Get the device descriptor
    auto err = libusb_get_device_descriptor(dev, &desc);
    if (err) {
        SPDLOG_ERROR("Cannot get device descriptor: {}", libusb_strerror(err));
        return;
    }
    // Print device information
    std::string device_name;
    if (skip_name) {
        device_name = std::format("{:04x}:{:04x}", desc.idVendor, desc.idProduct);
    } else {
        auto [mfr, product] = get_device_names(dev);
        device_name = std::format("{}-{}", mfr, product);
    }
    auto busid = get_device_busid(dev);
    bool is_used = false;
    bool is_available = false;
    {
        std::shared_lock lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        if (server_using_devices.contains(busid)) {
            is_used = true;
        }
        for (auto & server_available_device : server_available_devices) {
            if (server_available_device->busid == busid) {
                is_available = true;
            }
        }
    }
    std::cout << std::format("Device name: {} ({})", device_name,
                             is_used ? "exported" : (is_available ? "available" : "unbinded"))
              << std::endl;
    std::cout << std::format("busid: {}", busid) << std::endl;
    std::cout << std::format("  VID: 0x{:2x}", desc.idVendor) << std::endl;
    std::cout << std::format("  PID: 0x{:2x}", desc.idProduct) << std::endl;
    auto version = Version(desc.bcdUSB);
    std::cout << std::format("  USB version: {}.{}.{}", version.major, version.minor, version.patch) << std::endl;
    std::cout << std::format("  Class: 0x{:2x}", static_cast<int>(desc.bDeviceClass)) << std::endl;
    std::cout << "  Speed: ";
    // Parse device speed
    switch (libusb_get_device_speed(dev)) {
        case LIBUSB_SPEED_LOW:
            std::cout << "1.5 Mbps (Low)";
            break;
        case LIBUSB_SPEED_FULL:
            std::cout << "12 Mbps (Full)";
            break;
        case LIBUSB_SPEED_HIGH:
            std::cout << "480 Mbps (High)";
            break;
        case LIBUSB_SPEED_SUPER:
            std::cout << "5 Gbps (Super)";
            break;
        case LIBUSB_SPEED_SUPER_PLUS:
            std::cout << "10 Gbps (Super+)";
            break;
        default:
            std::cout << "Unknown speed";
    }
    std::cout << std::endl;
}

void LibusbServer::list_host_devices() {
    libusb_device **devs = nullptr;
    auto dev_nums = libusb_get_device_list(nullptr, &devs);
    if (dev_nums < 0) {
        // On failure devs is not assigned (remains nullptr), so do not proceed to
        // libusb_free_device_list(devs, 1) (freeing an uninitialized pointer is undefined behavior)
        SPDLOG_ERROR("libusb_get_device_list failed: {}", libusb_strerror(static_cast<int>(dev_nums)));
        return;
    }
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        if (config.skip_hub) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(devs[dev_i], &desc) == 0 &&
                desc.bDeviceClass == LIBUSB_CLASS_HUB)
                continue;
        }
        print_device(devs[dev_i]);
        std::cout << std::endl;
    }
    libusb_free_device_list(devs, 1);
}

libusb_device *LibusbServer::find_by_busid(const std::string &busid) {
    libusb_device **devs = nullptr;
    int dev_nums = libusb_get_device_list(nullptr, &devs);
    if (dev_nums < 0) {
        // On failure devs is not assigned; do not execute free below (see the list_host_devices comment)
        SPDLOG_ERROR("libusb_get_device_list failed: {}", libusb_strerror(dev_nums));
        return nullptr;
    }
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        if (get_device_busid(devs[dev_i]) == busid) {
            auto ret_dev = libusb_ref_device(devs[dev_i]);
            libusb_free_device_list(devs, 1);
            return ret_dev;
        }
    }
    libusb_free_device_list(devs, 1);
    return nullptr;
}

DeviceOperationResult LibusbServer::bind_host_device(libusb_device *dev) {
    if (dev == nullptr) {
        SPDLOG_ERROR("dev must not be null");
        return DeviceOperationResult::DeviceNotFound;
    }

    // Get the device descriptor
    libusb_device_descriptor device_descriptor{};
    int err = libusb_get_device_descriptor(dev, &device_descriptor);
    if (err) {
        SPDLOG_WARN("Cannot get device descriptor: {}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // Skip hub devices (bDeviceClass == 0x09); hubs are meaningless for USB/IP
    if (config.skip_hub && device_descriptor.bDeviceClass == LIBUSB_CLASS_HUB) {
        SPDLOG_DEBUG("Skipping hub device: busid={}", get_device_busid(dev));
        libusb_unref_device(dev);
        return DeviceOperationResult::HubFiltered;
    }

    // User filter: after descriptors, before opening the device/claiming interfaces; only read-only
    // queries have occurred, so skipping leaves no state behind. Combined with skip_hub rather than replacing it
    if (config.device_bind_filter && config.device_bind_filter(dev, device_descriptor)) {
        SPDLOG_DEBUG("Device skipped by device_bind_filter: busid={}", get_device_busid(dev));
        libusb_unref_device(dev);
        return DeviceOperationResult::DeviceFiltered;
    }

    // Get the configuration descriptor
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(dev, &active_config_desc);
    if (err) {
        SPDLOG_WARN("Cannot get the device's current configuration descriptor: {}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetConfigFailed;
    }

    // Build interface information
    SPDLOG_DEBUG("This device has {} interfaces", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("Interface {} has {} alternate settings", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .interface_number = intf.altsetting[0].bInterfaceNumber,
                                             .endpoints = std::move(endpoints)});
    }

    // Create UsbDevice and LibusbDeviceHandler. busid duplicate checking and insertion are
    // performed atomically under the same lock by Server::add_device, which also issues on_device_added
    // outside the lock on success; the backend does not operate on the device container directly
    auto current_device = std::make_shared<UsbDevice>(UsbDevice{
            .path = std::format("/sys/bus/{}/{}/{}", libusb_get_bus_number(dev), libusb_get_device_address(dev),
                                libusb_get_port_number(dev)),
            .busid = get_device_busid(dev),
            .bus_num = libusb_get_bus_number(dev),
            // Fill devnum with the port number, matching the reference project usbipd-libusb
            // (usbip_host_driver.c: udev->devnum = libusb_get_port_number(dev)).
            // This field is only displayed by usbip list; import matching uses busid, so functionality is unaffected;
            // kernel usbipd's devnum is the device address (the sysfs devnum attribute). Their
            // semantics differ but neither is relied upon by the client
            .dev_num = libusb_get_port_number(dev),
            .speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(dev)),
            .vendor_id = device_descriptor.idVendor,
            .product_id = device_descriptor.idProduct,
            .device_bcd = device_descriptor.bcdDevice,
            .device_class = device_descriptor.bDeviceClass,
            .device_subclass = device_descriptor.bDeviceSubClass,
            .device_protocol = device_descriptor.bDeviceProtocol,
            .configuration_value = active_config_desc->bConfigurationValue,
            .num_configurations = device_descriptor.bNumConfigurations,
            .interfaces = std::move(interfaces),
            .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
            .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
    });

    // Normal mode: pass the device reference (handler owns the reference)
    current_device->with_handler<LibusbDeviceHandler>(dev);
    const auto add_status = server.add_device(std::move(current_device));
    libusb_free_config_descriptor(active_config_desc);
    if (add_status != AddDeviceStatus::Added) {
        // Not added due to busid conflict: the device was destroyed within add_device, and handler destruction releases
        // the libusb reference (equivalent to the previous manual unref here)
        return add_status == AddDeviceStatus::InUse ? DeviceOperationResult::DeviceInUse
                                                    : DeviceOperationResult::DeviceAlreadyBound;
    }

    const auto busid = get_device_busid(dev);
    SPDLOG_INFO("Device {} added to the available list", busid);
    log_device_state(server);
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::bind_host_device_with_wrapped_fd(intptr_t fd) {
    if (fd < 0) {
        SPDLOG_ERROR("fd is invalid");
        return DeviceOperationResult::DeviceNotFound;
    }

    // Android mode: temporarily wrap fd to get device information, then close
    libusb_device_handle *temp_handle = nullptr;
    int err = libusb_wrap_sys_device(nullptr, fd, &temp_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        return DeviceOperationResult::DeviceOpenFailed;
    }

    // Get device information from the temporary handle
    libusb_device *device_for_info = libusb_get_device(temp_handle);
    if (!device_for_info) {
        SPDLOG_ERROR("libusb_get_device returns nullptr");
        libusb_close(temp_handle);
        return DeviceOperationResult::DeviceNotFound;
    }

    // Get the device descriptor
    libusb_device_descriptor device_descriptor{};
    err = libusb_get_device_descriptor(device_for_info, &device_descriptor);
    if (err) {
        SPDLOG_WARN("Cannot get device descriptor: {}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // Get the configuration descriptor
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(device_for_info, &active_config_desc);
    if (err) {
        SPDLOG_WARN("Cannot get the device's current configuration descriptor: {}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetConfigFailed;
    }

    // Build interface information
    SPDLOG_DEBUG("This device has {} interfaces", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("Interface {} has {} alternate settings", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .interface_number = intf.altsetting[0].bInterfaceNumber,
                                             .endpoints = std::move(endpoints)});
    }

    // Save device information
    auto busid = get_device_busid(device_for_info);
    auto bus_num = libusb_get_bus_number(device_for_info);
    auto dev_addr = libusb_get_device_address(device_for_info);
    // Fill devnum with the port number (as in normal mode; see the bind_host_device comment)
    auto dev_num = dev_addr;
    auto speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(device_for_info));
    auto configuration_value = active_config_desc->bConfigurationValue;

    // Close the temporary handle
    libusb_free_config_descriptor(active_config_desc);
    libusb_close(temp_handle);

    // Create UsbDevice and LibusbDeviceHandler; busid duplicate checking and insertion are
    // performed atomically under the same lock by Server::add_device (see the bind_host_device comment)
    auto current_device = std::make_shared<UsbDevice>(UsbDevice{
            .path = std::format("/sys/bus/{}/{}/{}", bus_num, dev_addr, dev_num),
            .busid = busid,
            .bus_num = bus_num,
            .dev_num = dev_num,
            .speed = speed,
            .vendor_id = device_descriptor.idVendor,
            .product_id = device_descriptor.idProduct,
            .device_bcd = device_descriptor.bcdDevice,
            .device_class = device_descriptor.bDeviceClass,
            .device_subclass = device_descriptor.bDeviceSubClass,
            .device_protocol = device_descriptor.bDeviceProtocol,
            .configuration_value = configuration_value,
            .num_configurations = device_descriptor.bNumConfigurations,
            .interfaces = std::move(interfaces),
            .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
            .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
    });

    // Android mode: pass fd (wrap it again on each connection)
    current_device->with_handler<LibusbDeviceHandler>(fd);
    const auto add_status = server.add_device(std::move(current_device));
    if (add_status != AddDeviceStatus::Added) {
        // Not added due to busid conflict (Android mode has no device reference to release; the caller holds fd)
        return add_status == AddDeviceStatus::InUse ? DeviceOperationResult::DeviceInUse
                                                    : DeviceOperationResult::DeviceAlreadyBound;
    }

    SPDLOG_INFO("Device {} added to the available list (fd={})", busid, fd);
    log_device_state(server);
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::unbind_host_device(libusb_device *device) {
    auto target_busid = get_device_busid(device);

    // Remove from the available list: Server::detach_available_device removes under the lock and issues
    // on_device_released(DeviceRemoved) outside it (explicit backend unbinding has the same meaning as physical removal)
    if (auto detached = server.detach_available_device(target_busid)) {
        if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(detached->handler)) {
            // Release interfaces if already claimed
            if (handler->interfaces_claimed_) {
                handler->release_and_close_device();
            }
            // Release the device reference
            if (handler->native_device_) {
                libusb_unref_device(handler->native_device_);
                handler->native_device_ = nullptr;
            }
        }
        libusb_unref_device(device);
        spdlog::info("Successfully unbound");
        log_device_state(server);
        return DeviceOperationResult::Success;
    }

    // Not in the available list: distinguish in use from not found
    auto result = DeviceOperationResult::DeviceNotFound;
    {
        std::shared_lock lock(server.get_devices_mutex());
        if (server.get_using_devices().contains(target_busid)) {
            SPDLOG_WARN("Devices in use cannot be unbound");
            result = DeviceOperationResult::DeviceInUse;
        }
    }
    if (result == DeviceOperationResult::DeviceNotFound) {
        SPDLOG_WARN("Target device is absent from available devices");
    }
    libusb_unref_device(device);
    log_device_state(server);
    return result;
}

DeviceOperationResult LibusbServer::unbind_host_device_by_fd(intptr_t fd) {
    // First find which busid in the available list corresponds to this fd (detach_available_device operates by
    // busid). Query and removal acquire the lock separately: unbinding is infrequent, and intervening state changes are harmless
    std::string target_busid;
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &dev: server.get_available_devices()) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(dev->handler)) {
                if (handler->wrapped_fd_ == fd) {
                    target_busid = dev->busid;
                    break;
                }
            }
        }
    }

    // Remove from the available list: remove under the lock and issue on_device_released(DeviceRemoved) outside it
    if (!target_busid.empty()) {
        if (auto detached = server.detach_available_device(target_busid)) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(detached->handler)) {
                // Release interfaces if already claimed
                if (handler->interfaces_claimed_) {
                    handler->release_and_close_device();
                }
                // Android mode has no native_device_; no release needed
            }
            SPDLOG_INFO("Successfully unbound device {} (fd={})", target_busid, fd);
            log_device_state(server);
            return DeviceOperationResult::Success;
        }
    }

    // Not found in the available list: check whether it is in use
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &entry: server.get_using_devices()) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(entry.second->handler)) {
                if (handler->wrapped_fd_ == fd) {
                    SPDLOG_WARN("Device is in use; unbinding unsupported (fd={})", fd);
                    return DeviceOperationResult::DeviceInUse;
                }
            }
        }
    }
    SPDLOG_WARN("No device found for fd={}", fd);
    return DeviceOperationResult::DeviceNotFound;
}

DeviceOperationResult LibusbServer::try_remove_dead_device(const std::string &busid) {
    // Try the available list first: Server::detach_available_device removes under the lock and notifies outside it.
    // One difference from the old implementation: no longer remove only if handler is LibusbDeviceHandler;
    // removal means the device has disappeared (leaving it behind makes Server keep considering it available)
    if (auto detached = server.detach_available_device(busid)) {
        if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(detached->handler)) {
            // The device may not be open
            if (libusb_device_handler->native_handle) {
                libusb_close(libusb_device_handler->native_handle);
                libusb_device_handler->native_handle = nullptr;
            }
            // Release the device reference
            if (libusb_device_handler->native_device_) {
                libusb_unref_device(libusb_device_handler->native_device_);
                libusb_device_handler->native_device_ = nullptr;
            }
        }
        spdlog::info("Removing {} from available devices", busid);
        return DeviceOperationResult::Success;
    }

    // Not in the available list: if in use, trigger disconnection (notification is issued by release_device during session cleanup)
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        if (auto device = server_using_devices.find(busid); device != server_using_devices.end()) {
            // Device is in use: do not directly close handle / unref device; pending
            // libusb transfer callbacks hold a raw handler pointer. After erase, a zero handler reference count
            // causes destruction, so subsequent callback access is use-after-free; also, session is unaware of device
            // removal. Follow the handle_device_left removal path: set
            // device_removed and trigger session stop, letting session cleanup (on_disconnection)
            // perform full cleanup: cancel transfers, wait for callbacks, release interfaces. The device is then removed from the
            // using list according to is_device_removed(), without returning to the available list
            if (std::dynamic_pointer_cast<LibusbDeviceHandler>(device->second->handler)) {
                device->second->handler->on_device_removed();
                SPDLOG_WARN("Device in use was removed; forcibly closing Session: {}", busid);
                device->second->handler->trigger_session_stop();
                return DeviceOperationResult::Success;
            }
        }
    }

    SPDLOG_WARN("Cannot find device with busid {}", busid);
    return DeviceOperationResult::DeviceNotFound;
}

// Reserved API: currently no internal call site (hotplug removal is handled by handle_device_left,
// with essentially the same logic); retained for external callers as needed
DeviceOperationResult LibusbServer::notify_device_removed(const std::string &busid) {
    SPDLOG_INFO("Device removed: {}", busid);

    // 1. Try the available list first: Server::detach_available_device removes the device under the lock and issues
    // on_device_released(DeviceRemoved) outside it
    if (auto detached = server.detach_available_device(busid)) {
        if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(detached->handler)) {
            if (handler->native_handle) {
                libusb_close(handler->native_handle);
                handler->native_handle = nullptr;
            }
            if (handler->native_device_) {
                libusb_unref_device(handler->native_device_);
                handler->native_device_ = nullptr;
            }
        }
        SPDLOG_INFO("Removed from the bound-device list: {}", busid);
        return DeviceOperationResult::Success;
    }

    // 2. Not in the available list: if in use, trigger disconnection (notification is issued by release_device during session cleanup)
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &using_devices = server.get_using_devices();
        if (auto it = using_devices.find(busid); it != using_devices.end()) {
            if (auto handler = it->second->handler) {
                handler->on_device_removed();
                SPDLOG_WARN("Device in use was removed; forcibly closing Session: {}", busid);
                handler->trigger_session_stop();
            }
            return DeviceOperationResult::Success;
        }
    }

    SPDLOG_WARN("Device not found: {}", busid);
    return DeviceOperationResult::DeviceNotFound;
}

void LibusbServer::start_hotplug_monitor() {
    if (!hotplug_enabled_by_user_) {
        SPDLOG_DEBUG("Hotplug monitoring disabled by the user");
        return;
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        SPDLOG_WARN("Current libusb does not support hotplug");
        return;
    }

    int ret = libusb_hotplug_register_callback(
            nullptr,
            static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback, this, &hotplug_handle_);

    if (ret == 0) {
        hotplug_enabled_ = true;
        SPDLOG_INFO("Hotplug monitoring started");
    }
    else {
        SPDLOG_ERROR("Failed to register hotplug callback: {}", libusb_strerror(ret));
    }
}

void LibusbServer::stop_hotplug_monitor() {
    if (hotplug_enabled_) {
        libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);
        hotplug_enabled_ = false;
        SPDLOG_INFO("Hotplug monitoring stopped");
    }
}

int LIBUSB_CALL LibusbServer::hotplug_callback(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event,
                                               void *user_data) {
    auto *server = static_cast<LibusbServer *>(user_data);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        server->handle_device_arrived(device);
    }
    else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        auto busid = get_device_busid(device);
        server->handle_device_left(busid);
    }

    return 0;
}

void LibusbServer::handle_device_arrived(libusb_device *device) {
    auto busid = get_device_busid(device);

    // Check whether already bound
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &dev: server.get_available_devices()) {
            if (dev->busid == busid) {
                SPDLOG_DEBUG("Device {} is already in the bound list", busid);
                return;
            }
        }
        if (server.get_using_devices().contains(busid)) {
            SPDLOG_DEBUG("Device {} is in use", busid);
            return;
        }
    }

    // Print device information (hotplug callbacks run on the libusb event thread; always skip device-name
    // queries, since get_device_names calls libusb_open and may block event processing on the event thread;
    // displaying VID:PID suffices. Device-name queries are used only in list_host_devices on the main thread)
    SPDLOG_INFO("New device insertion detected:");
    print_device(device, true);

    // Automatic binding's bind_host_device calls libusb_get_active_config_descriptor:
    // on the main platform Linux (usbfs backend), this is a synchronous ioctl that does not use the libusb event
    // loop, so calling it in a hotplug callback is safe. Backends whose synchronous I/O uses the event loop, such as darwin (macOS),
    // theoretically risk nested event processing, but this project has no real-device runtime scenarios on that platform.
    // Do not dispatch binding to a worker thread: this adds queue/thread/lifetime-management complexity, is difficult on embedded
    // platforms, and the main platform's behavior has already been verified safe
    if (config.auto_bind_hotplug) {
        SPDLOG_INFO("Automatically binding new device: {}", busid);
        libusb_ref_device(device); // bind_host_device takes over the reference
        auto result = bind_host_device(device);
        if (result == DeviceOperationResult::Success) {
            SPDLOG_INFO("Automatic binding succeeded: {}", busid);
        } else if (result == DeviceOperationResult::HubFiltered) {
            SPDLOG_DEBUG("New device {} is a hub; skipping", busid);
        } else if (result == DeviceOperationResult::DeviceFiltered) {
            SPDLOG_DEBUG("New device {} skipped by device_bind_filter", busid);
        } else {
            SPDLOG_WARN("Automatic binding failed: {}", busid);
        }
    }
}

void LibusbServer::handle_device_left(const std::string &busid) {
    SPDLOG_INFO("Device removal detected: {}", busid);

    // 1. Try the available list first: Server::detach_available_device removes the device under the lock and issues
    // on_device_released(DeviceRemoved) outside it; the backend neither operates on the container directly nor issues its own notification
    if (auto detached = server.detach_available_device(busid)) {
        if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(detached->handler)) {
            // The device may not be open
            if (handler->native_handle) {
                libusb_close(handler->native_handle);
                handler->native_handle = nullptr;
            }
            if (handler->native_device_) {
                libusb_unref_device(handler->native_device_);
                handler->native_device_ = nullptr;
            }
        }
        SPDLOG_INFO("Removed from the bound-device list: {}", busid);
        // The device has been removed; cleaning it up now requires no lock (other threads can no longer obtain it)
        return;
    }

    // 2. Not in the available list: if in use, trigger disconnection. This path issues no notification: the device stays in the
    // using list and release_device discards it during session cleanup according to is_device_removed(),
    // issuing on_device_released(DeviceRemoved)
    std::lock_guard lock(server.get_devices_mutex());
    auto &using_devices = server.get_using_devices();
    if (auto it = using_devices.find(busid); it != using_devices.end()) {
        if (auto handler = it->second->handler) {
            // Notify through the AbstDeviceHandler interface (backend-independent)
            handler->on_device_removed();
            // Forcibly close Session
            SPDLOG_WARN("Device in use was removed; forcibly closing Session: {}", busid);
            handler->trigger_session_stop();
        }
    }
}

void LibusbServer::bind_existing_devices() {
    libusb_device **devs = nullptr;
    ssize_t dev_nums = libusb_get_device_list(nullptr, &devs);
    if (dev_nums < 0) {
        SPDLOG_ERROR("libusb_get_device_list failed: {}", libusb_strerror(static_cast<int>(dev_nums)));
        return;
    }
    int bound = 0, skipped = 0;
    for (ssize_t i = 0; i < dev_nums; i++) {
        auto busid = get_device_busid(devs[i]);
        // Skip already bound or in-use devices
        {
            std::shared_lock lock(server.get_devices_mutex());
            bool already_known = server.get_using_devices().contains(busid);
            if (!already_known) {
                for (const auto &d : server.get_available_devices()) {
                    if (d->busid == busid) { already_known = true; break; }
                }
            }
            if (already_known) continue;
        }
        libusb_ref_device(devs[i]); // bind_host_device takes over the reference
        auto result = bind_host_device(devs[i]);
        switch (result) {
            case DeviceOperationResult::Success: bound++; break;
            case DeviceOperationResult::HubFiltered: skipped++; break;
            case DeviceOperationResult::DeviceFiltered: skipped++; break;
            default: break;
        }
    }
    // Pass 1 as the second argument: the list itself holds a reference to each device, which must be released.
    // Successfully bound devices have a separate libusb_ref_device reference taken over by handler (unref on destruction), without conflict.
    // Passing 0 leaks one reference per device, preventing libusb_device destruction forever
    libusb_free_device_list(devs, 1);
    SPDLOG_INFO("Binding scan complete: {} bound, {} skipped (hub or device_bind_filter)", bound, skipped);
}

usbipdcpp::error_code LibusbServer::start(const asio::ip::tcp::endpoint &ep) {
    start_hotplug_monitor();

    should_exit_libusb_event_thread = false;

    // On thread creation failure (insufficient system resources), std::thread construction throws; start promises
    // not to throw (errors reported through error_code), so catch, clean up started hotplug monitoring, and return
    // an error code (matching Server::start's thread creation failure handling)
    try {
        libusb_event_thread = std::thread([this]() {
            try {
                SPDLOG_INFO("Start a libusb event-loop thread for a libusb device handle");
                while (!should_exit_libusb_event_thread) {
                    auto ret = libusb_handle_events(nullptr);

                    if (ret == LIBUSB_ERROR_INTERRUPTED && should_exit_libusb_event_thread) [[unlikely]] {
                        SPDLOG_INFO("libusb event loop received an interrupt signal and exited normally");
                        break;
                    }
                    if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED) [[unlikely]] {
                        SPDLOG_ERROR("Event handling error: {}\n", libusb_strerror(ret));
                        break;
                    }
                }
                SPDLOG_TRACE("Exiting libusb event loop");
            } catch (const std::exception &e) {
                // An event-loop exception (e.g. corrupted internal libusb state) is unrecoverable: all
                // transfer callbacks depend on this loop for dispatch; if the loop dies, session disconnection cleanup
                // (on_disconnection waiting for pending to reach zero) waits forever, making graceful exit
                // impossible. Failing fast with std::exit is the only safe choice (the alternative,
                // exiting the thread, hangs every session and is worse than crashing); leave the exit code to
                // the host process's monitoring/watchdog
                SPDLOG_ERROR("An unexpected exception occurs in libusb handler thread: {}", e.what());
                std::exit(1);
            }
        });
    } catch (...) {
        stop_hotplug_monitor();
        SPDLOG_ERROR("libusb event thread creation failed");
        return std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    // Server::start does not throw; errors are reported through the return value (for embedded platforms without exceptions)
    auto ec = server.start(ep);
    if (ec) [[unlikely]] {
        // When start fails (e.g. the port is occupied), callers treat it as failure and do not call stop(); reclaim
        // already started resources here: deregister hotplug monitoring and stop the libusb event thread (join),
        // otherwise the thread remains joinable and LibusbServer destruction (empty destructor) triggers std::terminate
        stop_hotplug_monitor();
        should_exit_libusb_event_thread = true;
        libusb_interrupt_event_handler(nullptr);
        if (libusb_event_thread.joinable()) {
            libusb_event_thread.join();
        }
    }
    return ec;
}

void LibusbServer::stop() {
    stop_hotplug_monitor();
    // Order matters: server.stop() (interrupt session -> on_disconnection waits for
    // libusb transfer callbacks) must precede stopping the event thread, since session disconnection cleanup requires
    // the event thread to keep processing callbacks (callbacks after cancel run on the event thread, allowing the pending count
    // to reach zero). Stopping the event thread first makes on_disconnection wait forever. Interrupt and join the event thread
    // last. Hotplug deregistration does not wait for a callback already executing,
    // but callbacks only operate on lists protected by devices_mutex, mutually exclusive with cleanup below
    server.stop();
    SPDLOG_INFO("usbip server closed");

    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_available_devices.clear();

        for (auto using_dev_i = server_using_devices.begin(); using_dev_i != server_using_devices.end();
             ++using_dev_i) {
            if (auto libusb_device_handler =
                        std::dynamic_pointer_cast<LibusbDeviceHandler>(using_dev_i->second->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_using_devices.clear();
    }

    should_exit_libusb_event_thread = true;
    libusb_interrupt_event_handler(nullptr);
    spdlog::info("Waiting for libusb event thread to end");
    // Safeguard: if start failed (already joined internally) or was never called, the thread is not joinable;
    // joining directly would cause std::terminate
    if (libusb_event_thread.joinable()) {
        libusb_event_thread.join();
    }
    spdlog::info("libusb event thread ended");
}

LibusbServer::~LibusbServer() {
}
