#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {
// Construct a virtual keyboard device (same construction as examples/mock_keyboard).
// Caller must own string_pool and outlive device (handler retains its reference)
std::shared_ptr<UsbDevice> make_mock_keyboard(StringPool &string_pool) {
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
    mock_keyboard->interfaces[0].with_handler<KeyboardHandler>(string_pool);
    auto device_handler = mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();
    return mock_keyboard;
}

} // namespace

TEST(TestNetworkVdev, ServerCanStopWithImportedDevice) {
    // At stop(), session is in transfer state (receiver/sender threads after device import):
    // immediately_stop must interrupt receiver; device must return to available list; stop() returns only after all session
    // threads join, ensuring no thread still accesses Server when it is subsequently destroyed
    asio::io_context io;

    // string_pool must be declared before server (destroyed after server); handler retains its reference
    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    for (int round = 0; round < 2; round++) {
        ASSERT_FALSE(server.start(ep));
        // Start on port 0; query actual listening endpoint (including system-allocated port) using endpoint()
        auto actual_ep = server.endpoint();

        // Connect to server, polling until listening is ready
        asio::ip::tcp::socket client(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            client.connect(actual_ep, ec);
            if (!ec) {
                connected = true;
                break;
            }
            client.close();
            client = asio::ip::tcp::socket(io);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);

        // Send import request, putting session in transfer state
        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        // Read OpRepImport response header (version + command + status), confirming successful import
        std::uint16_t version = 0;
        std::uint16_t command = 0;
        std::uint32_t status = 0;
        data_read_from_socket(client, version, command, status);
        ASSERT_EQ(command, OP_REP_IMPORT);
        ASSERT_EQ(status, 0u);

        // Without closing client, call stop() directly: immediately_stop must interrupt transferring session
        server.stop();
        client.close();
    }
}

// ---------------------------------------------------------------------------
// Various unusual client connection/disconnection scenarios follow
// ---------------------------------------------------------------------------

TEST(TestNetworkVdev, ClientRstWithoutAnyData) {
    // RST directly after connection without sending data: session blocked reading in parse_op; read must
    // return connection-reset error (SOCKET_ERR path, distinct from FIN's EOF path),
    // session exits cleanly; stop() does not hang
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        rst_disconnect(client);
    }
    // Session thread must clean itself up after client disconnection
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientSendsHalfOpThenDisconnects) {
    // Send only 1 byte then disconnect: server reading 2-byte version is stuck on half a header,
    // followed by EOF. Most extreme "partial-packet disconnection"; session must exit cleanly
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        const std::array<std::uint8_t, 1> half_header = {0x01};
        asio::write(client, asio::buffer(half_header));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsRightAfterImportRequest) {
    // RST after import request without awaiting server reply: server may discover disconnection while opening device or writing
    // reply. Session must exit cleanly; device must not remain stuck in using_devices—
    // reimporting the same device after restart must succeed
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        rst_disconnect(client); // RST directly without reading reply
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // Reimport same device after restart: succeeds only if device not stuck in using_devices
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsAfterSuccessfulImport) {
    // After successful import, client gracefully disconnects (FIN): receiver reads EOF, taking full cleanup path
    // (on_disconnection → device returns to available list). 3 rounds verify repeatable device
    // import-release without leaks in using_devices
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    for (int round = 0; round < 3; round++) {
        ASSERT_FALSE(server.start(ep));
        {
            asio::ip::tcp::socket client(io);
            ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
            ASSERT_EQ(import_device(client, "1-1"), 0u);
            client.close(); // Graceful disconnection during transfer
        }
        ASSERT_TRUE(wait_sessions_gone(server));
        server.stop();
    }
}

TEST(TestNetworkVdev, ClientRstDuringTransfer) {
    // After successful import, client sends RST during transfer (no FIN): receiver's pending read must return
    // connection-reset error, taking full cleanup path; device released and available for reimport
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        rst_disconnect(client); // RST during transfer
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // Reimport after restart verifies device release
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, TwoClientsContendForSameDevice) {
    // Two clients contend for same device: after A imports successfully, B importing same device must receive NA;
    // only after A disconnects and releases device can C import successfully
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));

    // A imports successfully first
    asio::ip::tcp::socket client_a(io);
    ASSERT_TRUE(connect_with_retry(client_a, server.endpoint()));
    ASSERT_EQ(import_device(client_a, "1-1"), 0u);

    // B imports same device → NA
    asio::ip::tcp::socket client_b(io);
    ASSERT_TRUE(connect_with_retry(client_b, server.endpoint()));
    ASSERT_EQ(import_device(client_b, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NA));

    // A disconnects; device released
    client_a.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    // C imports successfully afterward
    asio::ip::tcp::socket client_c(io);
    ASSERT_TRUE(connect_with_retry(client_c, server.endpoint()));
    ASSERT_EQ(import_device(client_c, "1-1"), 0u);

    client_c.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientSendsGarbageThenDisconnects) {
    // Send meaningless byte stream then disconnect: parsing either reports unknown version/command or read is interrupted;
    // server must not crash; session exits cleanly
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        const std::array<std::uint8_t, 16> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03,
                                                      0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
        asio::write(client, asio::buffer(garbage));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, StopRightAfterImportRequest) {
    // stop() after client sends import request but before server replies (client remains connected):
    // immediately_stop must interrupt session at any stage of import handling.
    // Key assertion: after restart, device not in using_devices (no leak), can be imported again
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    {
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        server.stop(); // Client still connected; stop directly without reading reply
    }
    client.close();

    // Reimport after restart: receives NA if device stuck in using_devices
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsDuringUrbTransfer) {
    // Client sends real URB (CMD_SUBMIT, keyboard interrupt IN endpoint) then disconnects immediately:
    // connection breaks while device handler processes URB or sender writes RET_SUBMIT response.
    // Covers device-side in-flight URB cleanup (on_disconnection) and sender write-failure branch;
    // device must be released and reimportable after restart
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);

        // Construct URB for keyboard interrupt IN endpoint (logical ep=1, converted server-side to 0x81)
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 1;
        submit.header.devid = 1;
        submit.header.direction = UsbIpDirection::In;
        submit.header.ep = 0x01;
        submit.transfer_flags = 0;
        submit.transfer_buffer_length = 8;
        submit.start_frame = 0;
        submit.number_of_packets = 0;
        submit.interval = 0;
        usbipdcpp::error_code send_ec;
        submit.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        rst_disconnect(client); // RST during URB processing, without reading RET_SUBMIT response
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // Reimport after restart verifies device release
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ImportNonexistentDevice) {
    // Client imports nonexistent device: server replies NoDev; session cleans up normally
    // without crashing; subsequent import of real device still works
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "9-9"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // Real device unaffected
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientReconnectLoop) {
    // Without restarting server, client repeatedly imports→disconnects→reconnects→imports same device:
    // device must return correctly to available list every time. Alternate FIN and RST disconnections;
    // server still works normally after 10 rounds
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    for (int round = 0; round < 10; round++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u) << "Round " << round << " import failed";
        if (round % 2 == 0) {
            client.close(); // Graceful disconnection
        }
        else {
            rst_disconnect(client); // RST disconnection
        }
        ASSERT_TRUE(wait_sessions_gone(server)) << "Round " << round << " session not cleaned up";
    }
    server.stop();
}

