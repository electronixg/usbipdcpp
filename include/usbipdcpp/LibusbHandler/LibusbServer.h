#pragma once

#include <asio.hpp>
#include <functional>
#include <libusb-1.0/libusb.h>

#include "usbipdcpp/Server.h"

namespace usbipdcpp {

/**
 * @brief Result of device bind/unbind operations.
 */
enum class DeviceOperationResult {
    Success, ///< Operation completed successfully
    DeviceNotFound, ///< Device was not found in the device list
    DeviceInUse, ///< Device is currently in use and cannot be modified
    DeviceAlreadyBound, ///< Device with same busid is already bound
    DeviceOpenFailed, ///< Failed to open the device
    GetDescriptorFailed, ///< Failed to get device descriptor
    GetConfigFailed, ///< Failed to get configuration descriptor
    ClaimInterfaceFailed, ///< Failed to claim interface
    HubFiltered, ///< Hub device skipped by filter
    DeviceFiltered ///< Device skipped by the user-provided device_bind_filter
};

/**
 * @brief LibusbServer behavior configuration
 */
struct LibusbServerConfig {
    bool skip_hub = true; ///< Skip hub device (bDeviceClass == 0x09)
    bool auto_bind_hotplug = false; ///< Automatically bind new devices when hotplug

    /**
     * @brief device binding filter: returning true means not exporting the device (skipping binding)
     *
     * is called after getting the device descriptor and before opening the device (at this time there is only read-only query of libusb,
     * skips without leaving any residual status); bind_existing_devices and hotplug are automatically bound through
     * bind_host_device takes effect. skip_hub is judged first and this filter is judged later. The two are superimposed rather than mutually exclusive.
     * - Use this when you need to customize exclusion rules based on VID/PID, device class, etc. There is no need to change skip_hub.
     *
     * @param dev libusb device (read-only, do not unref in callback)
     * @param desc The device descriptor that has been obtained
     */
    std::function<bool(libusb_device *dev, const libusb_device_descriptor &desc)> device_bind_filter;
};

/**
 * @brief libusb-based USB/IP server
 *
 * @attention Thread safety summary:
 *   - Construct / start / stop / ~LibusbServer: life cycle method, must be called serially in the same thread
 *   - bind / unbind / try_remove_dead_device / notify_device_removed: Internally locked, safe from any thread
 *   - print_device / list_host_devices: Internal locking, safe from any thread
 *   - get_device_names / find_by_busid: static method, operating independent libusb device list, arbitrary thread safety
 *   - get_server: Always safe (only returns a reference), but calls to the returned object need to follow the Server's own thread constraints
 *   - set_hotplug_enabled / is_hotplug_enabled: must be called/read before start()
 */
class USBIPDCPP_API LibusbServer {
public:
    explicit LibusbServer(const LibusbServerConfig &config = {});
    LibusbServer(const LibusbServer &) = delete;
    LibusbServer(LibusbServer &&) = delete;
    LibusbServer &operator=(const LibusbServer &) = delete;
    LibusbServer &operator=(LibusbServer &&) = delete;

    /**
     * @brief Bind a physical USB device to make it available for export (normal mode).
     *
     * This function retrieves device information and adds it to the available devices list.
     * The device is not opened until a client connects (lazy binding).
     * On success the Server emits on_device_added for the device (see ServerObserver).
     *
     * @param dev The libusb device to bind. Must not be nullptr.
     *            The function takes ownership of the device reference (released on failure).
     * @return DeviceOperationResult::Success on success;
     *         DeviceAlreadyBound if that busid is already in the available list;
     *         DeviceInUse if that busid is currently used by a session;
     *         GetDescriptorFailed / GetConfigFailed on device checks;
     *         HubFiltered / DeviceFiltered when skip_hub or device_bind_filter
     *         says to skip this device.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult bind_host_device(libusb_device *dev);

    /**
     * @brief Bind a physical USB device with a system file descriptor (Android mode).
     *
     * This is for Android where the device is accessed via a file descriptor obtained
     * from UsbManager.openDevice(). The fd is wrapped via libusb_wrap_sys_device()
     * on each client connection, supporting reconnection after disconnection.
     * On success the Server emits on_device_added for the device (see ServerObserver).
     *
     * @param fd A valid file descriptor opened on the device node.
     *           The fd must remain valid until the device is unbound or the server is stopped.
     * @return DeviceOperationResult::Success on success;
     *         DeviceAlreadyBound if that busid is already in the available list;
     *         DeviceInUse if that busid is currently used by a session;
     *         GetDescriptorFailed / GetConfigFailed on device checks.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult bind_host_device_with_wrapped_fd(intptr_t fd);

    /**
     * @brief Unbind a previously bound physical USB device.
     *
     * Releases all interfaces, reattaches kernel drivers, closes the device handle,
     * and removes the device from the available devices list. The device reference
     * will be released. Removing from the list makes the Server emit
     * on_device_released(busid, DeviceRemoved) (see ServerObserver).
     *
     * @param device The libusb device to unbind. The function takes ownership of this reference.
     * @return DeviceOperationResult::Success on success,
     *         DeviceOperationResult::DeviceNotFound if not in available devices,
     *         DeviceOperationResult::DeviceInUse if currently in use.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult unbind_host_device(libusb_device *device);

    /**
     * @brief Unbind a previously bound device by its file descriptor (Android mode).
     *
     * Finds and removes the device that was bound with the specified fd.
     * On success the Server emits on_device_released(busid, DeviceRemoved) (see ServerObserver).
     *
     * @param fd The file descriptor used when binding the device.
     * @return DeviceOperationResult::Success on success,
     *         DeviceOperationResult::DeviceNotFound if not in available devices,
     *         DeviceOperationResult::DeviceInUse if currently in use.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult unbind_host_device_by_fd(intptr_t fd);

    /**
     * @brief Remove a dead device from the device lists.
     *
     * This function should not be called with a busid that is still in use.
     * A device found in the available list is removed unconditionally (whatever
     * handler type it carries — leaving it behind would make the Server keep
     * believing it is usable); if it is not in that list but currently in use,
     * the session is stopped instead and the device is dropped later by the
     * session teardown. Removing from the available list makes the Server emit
     * on_device_released(busid, DeviceRemoved) (see ServerObserver).
     *
     * @param busid The bus ID of the device to remove.
     * @return DeviceOperationResult::Success if found and removed,
     *         DeviceOperationResult::DeviceNotFound if not found.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult try_remove_dead_device(const std::string &busid);

    /**
     * @brief Notify that a device has been physically removed (Android mode).
     *
     * This should be called when the system detects a USB device has been detached.
     * If the device is currently in use, it will trigger disconnection and stop the session.
     * Removing from the available list makes the Server emit
     * on_device_released(busid, DeviceRemoved) (see ServerObserver).
     *
     * @param busid The bus ID of the removed device.
     * @return DeviceOperationResult::Success if found and handled,
     *         DeviceOperationResult::DeviceNotFound if not found.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult notify_device_removed(const std::string &busid);

    /**
     * @brief Start the server listening on the specified endpoint.
     *
     * Also starts the hotplug monitor if enabled and supported.
     *
     * @param ep The TCP endpoint to listen on.
     * @return The error code if the server fails to start (e.g. port already
     *         in use), no error otherwise. Does not throw.
     *
     * @thread_safety cannot be called concurrently. Called at most once (stop() is required before calling again).
     */
    usbipdcpp::error_code start(const asio::ip::tcp::endpoint &ep);

    /**
     * @brief Stop the server.
     *
     * Stops the hotplug monitor, closes all device handles, releases interfaces,
     * and reattaches kernel drivers.
     *
     * @thread_safety cannot be called concurrently. Must be called after start() and before destruction.
     */
    void stop();

    ~LibusbServer();

    /**
     * @brief Print detailed information about a USB device to stdout.
     *
     * Displays device name, bus ID, VID/PID, USB version, device class, and speed.
     * Also shows whether the device is exported, available, or unbound.
     *
     * @param dev The libusb device to print information about.
     * @param skip_name Skip device name lookup (avoids calling get_device_names which may
     *                  block on libusb_open). When true, VID:PID is printed instead.
     *
     * @thread_safety Internally locked to read device status, safe from any thread.
     */
    void print_device(libusb_device *dev, bool skip_name = false);

    /**
     * @brief List all USB devices connected to the host.
     *
     * Prints detailed information about each device to stdout.
     *
     * @thread_safety Use libusb global device list (libusb_get_device_list), safe from any thread.
     */
    void list_host_devices();

    /**
     * @brief Scans and binds all currently connected devices.
     *
     * automatically skips the hub device based on config.skip_hub.
     * Devices that are bound or in use will also be skipped. Can be called from any thread.
     */
    void bind_existing_devices();

    /**
     * @brief Get the underlying Server instance.
     *
     * shares the same listening port with the virtual device: the physical device enters the server through bind_*
     * available_devices, virtual device uses `get_server().add_device(virtual UsbDevice)`
     * can be added to the same Server - the Server has no awareness of the device backend (UsbDevice uniformly carries
     * LibusbDeviceHandler / VirtualDeviceHandler).
     *
     * @warning When mixing, ensure busid uniqueness: clients attach by busid; duplicate busid values
     *          match an indeterminate device. A virtual device's default busid is often "1-1", which may conflict with a physical
     *          device's bus number (such as "1-1"); assign the virtual device a separate,
     *          nonconflicting busid before mixing them.
     *
     * @return Reference to the internal Server object.
     *
     * @thread_safety Always safe (only returns a reference). Operations on the returned objects must follow the thread constraints of the Server.
     */
    Server &get_server() {
        return server;
    }

    /**
     * @brief Get the manufacturer and product names of a USB device.
     *
     * @warning This function calls libusb_open() which is blocking I/O. Avoid calling it
     *          from the libusb event thread (e.g. hotplug callback) or performance-critical
     *          paths. On platforms with a shared USB/Ethernet bus (e.g. Raspberry Pi 3B),
     *          concurrent libusb_open calls may cause network drops.
     *
     * @param device The libusb device to get names from.
     * @return A pair containing {manufacturer, product} names. Returns "Unknown Manufacturer"
     *         or "Unknown Product" if the corresponding string descriptor is not available.
     *
     * @thread_safety Static method, open an independent libusb handle, safe from any thread.
     */
    static std::pair<std::string, std::string> get_device_names(libusb_device *device);

    /**
     * @brief Find a libusb device by its bus ID.
     *
     * @param busid The bus ID to search for (e.g., "1-2.3").
     * @return A libusb_device pointer if found, nullptr otherwise. The caller must call
     *         libusb_unref_device() when done with the device.
     *
     * @thread_safety Static method, using libusb global device list, any thread safe.
     */
    static libusb_device *find_by_busid(const std::string &busid);

    /**
     * @brief Set whether hotplug monitoring is enabled.
     *
     * Hotplug monitoring is enabled by default. On Android without root privileges,
     * hotplug is not supported and should be disabled before starting the server.
     *
     * @param enabled true to enable hotplug monitoring, false to disable.
     *
     * @thread_safety must be called before start().
     */
    void set_hotplug_enabled(bool enabled) {
        hotplug_enabled_by_user_ = enabled;
    }

    /**
     * @brief Check if hotplug monitoring is enabled.
     *
     * @return true if hotplug monitoring is enabled, false otherwise.
     *
     * @thread_safety Read atomic variables, safe from any thread. But it should be valid after start().
     */
    bool is_hotplug_enabled() const {
        return hotplug_enabled_by_user_;
    }

protected:
    Server server;

    std::atomic<bool> should_exit_libusb_event_thread = false;

    // Network packets cannot be sent in this thread.
    std::thread libusb_event_thread;

    // hotplug related
    libusb_hotplug_callback_handle hotplug_handle_ = 0;
    bool hotplug_enabled_ = false;
    bool hotplug_enabled_by_user_ = true; // User-set switch, enabled by default

    LibusbServerConfig config;

    void start_hotplug_monitor();
    void stop_hotplug_monitor();

    void handle_device_arrived(libusb_device *device);
    void handle_device_left(const std::string &busid);

    static int LIBUSB_CALL hotplug_callback(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event,
                                            void *user_data);
};
} // namespace usbipdcpp
