// Dedicated release-semantics tests after physical device removal (is_device_removed()): removed before import,
// removed during transfer, FIN/RST disconnection, concurrent stop, contention between multiple clients—the device must always be discarded directly from the using
// list and never returned to the available list (the backend scans the list only once at unplug time; returning it creates
// a zombie that nobody can remove, merely causing a wasted failed open on the next import).
// The real libusb backend's flag-setting paths (hotplug / transfer callback / try_remove_dead_device)
// require real devices and are covered by e2e; virtual devices reproduce the same semantics here
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {
// Virtual keyboard that can be marked "physically removed": the device-level handler connects is_device_removed() to
// a shared flag; "open failure" resides at the interface layer—real device open failures (such as audio device open
// failures) happen here; the base VirtualDeviceHandler::on_new_connection already rolls back interface
// connection failures (stops scheduler, disconnects connected interfaces, withdraws responder); the failed
// interface itself established no state and therefore needs no rollback
class RemovableKeyboardInterfaceHandler : public KeyboardHandler {
public:
    using KeyboardHandler::KeyboardHandler;

    // The removal flag is held by the test and shared with the device-level handler (outliving the device)
    std::shared_ptr<std::atomic_bool> removed_flag;

    void on_new_connection(TransferResponder &responder, error_code &ec) override {
        if (removed_flag && removed_flag->load()) {
            // An unplugged device cannot be opened: no connection state is established, so no rollback is needed
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
        KeyboardHandler::on_new_connection(responder, ec);
    }
};

// Device-level handler: does not override on_new_connection (connection establishment and failure rollback both use the base class),
// only connects is_device_removed() to the shared flag
class RemovableDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    std::shared_ptr<std::atomic_bool> removed_flag;

    bool is_device_removed() const override {
        return removed_flag && removed_flag->load();
    }
};

// Construct a virtual keyboard device using the two handlers above (interface definition matches mock_keyboard)
std::shared_ptr<UsbDevice> make_removable_keyboard(StringPool &string_pool,
                                                   std::shared_ptr<std::atomic_bool> removed_flag) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    .max_packet_size = 16,
                                    .interval = 10,
                            },
                    }},
            },
    };
    auto mock_keyboard = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_keyboard",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0xABCD,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto interface_handler = mock_keyboard->interfaces[0].with_handler<RemovableKeyboardInterfaceHandler>(string_pool);
    interface_handler->removed_flag = removed_flag;
    auto device_handler = mock_keyboard->with_handler<RemovableDeviceHandler>(string_pool);
    device_handler->removed_flag = removed_flag;
    device_handler->setup_interface_handlers();
    return mock_keyboard;
}

// Assert the device has been discarded: both available and in-use lists are empty
void expect_discarded(Server &server) {
    std::shared_lock lock(server.get_devices_mutex());
    EXPECT_TRUE(server.get_available_devices().empty());
    EXPECT_TRUE(server.get_using_devices().empty());
}

// Run one "import → device removed during transfer → client disconnects" round; assert the device is discarded.
// use_rst selects the disconnection method (FIN / RST take different receiver cleanup branches)
void expect_removed_device_discarded(bool use_rst) {
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    {
        std::shared_lock lock(server.get_devices_mutex());
        EXPECT_TRUE(server.get_available_devices().empty());
        EXPECT_EQ(server.get_using_devices().size(), 1u);
    }

    removed_flag->store(true); // Device physically unplugged during transfer

    if (use_rst) {
        rst_disconnect(client);
    }
    else {
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    expect_discarded(server);
    server.stop();
}
} // namespace

TEST(TestDeviceRemoval, RemovedDeviceIsNotReturnedToAvailable) {
    expect_removed_device_discarded(false);
}

TEST(TestDeviceRemoval, RemovedDeviceIsNotReturnedToAvailableOnRst) {
    expect_removed_device_discarded(true);
}

TEST(TestDeviceRemoval, AliveDeviceIsReturnedToAvailable) {
    // Control case: disconnecting a device that has not been removed returns it to the available list as usual (discard logic must not harm normal devices)
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    {
        std::shared_lock lock(server.get_devices_mutex());
        EXPECT_EQ(server.get_available_devices().size(), 1u);
        EXPECT_TRUE(server.get_using_devices().empty());
    }
    server.stop();
}

TEST(TestDeviceRemoval, ImportRemovedDeviceFailsAndIsDiscarded) {
    // A removed device still remaining in the list (race window between release and flagging): import must
    // fail (opening an unplugged device cannot succeed); afterward the device is discarded directly rather than remaining
    // in the list for repeated imports—importing again through another connection yields only "device does not exist"
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(true); // Remaining in the list after unplugging
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NA));
    expect_discarded(server);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    asio::ip::tcp::socket retry_client(io);
    ASSERT_TRUE(connect_with_retry(retry_client, server.endpoint()));
    EXPECT_EQ(import_device(retry_client, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
    retry_client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestDeviceRemoval, StopWithRemovedImportedDevice) {
    // stop() interrupts the session of a transferring device that has been removed: discard the device from using as well,
    // without returning it to the available list through stop's cleanup path
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    removed_flag->store(true); // Device physically unplugged during transfer
    server.stop();             // Stop directly without closing the client; take the interruption path

    expect_discarded(server);
    client.close();
}

TEST(TestDeviceRemoval, SecondClientCannotImportDeviceInUseEvenIfRemoved) {
    // The device has been removed but the first client still holds it (session not cleaned up): the second client's import
    // must be rejected (device still in using), not allowed because it is "removed"; discard it after the first disconnects
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client_a(io);
    ASSERT_TRUE(connect_with_retry(client_a, server.endpoint()));
    ASSERT_EQ(import_device(client_a, "1-1"), 0u);

    removed_flag->store(true); // Device physically unplugged while in use

    asio::ip::tcp::socket client_b(io);
    ASSERT_TRUE(connect_with_retry(client_b, server.endpoint()));
    EXPECT_NE(import_device(client_b, "1-1"), 0u); // In use; reject
    client_b.close();

    client_a.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    expect_discarded(server); // Discard from using after disconnection; do not return to the available list
    server.stop();
}