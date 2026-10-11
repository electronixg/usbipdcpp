// Dedicated Server::set_connection_filter (connection-source admission control) tests: allow all by default when unset,
// rejection semantics (close connection without establishing a session, sending on_session_started, or occupying a device),
// source (source-port) filtering; filter exceptions treated as rejection (fail-closed) without killing the accept
// loop; persistence across start/stop; restoring admission after clearing
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

// Observer recording session-start events: rejected connections must not generate this event (callbacks execute on the session thread,
// with internal locking)
class StartedObserver : public ServerObserver {
public:
    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        started_count.fetch_add(1);
        std::lock_guard lock(mutex);
        peers.push_back(peer);
    }

    std::vector<std::string> peer_snapshot() const {
        std::lock_guard lock(mutex);
        return peers;
    }

    std::atomic<int> started_count{0};

private:
    mutable std::mutex mutex;
    std::vector<std::string> peers;
};

// Server must already be stopped before destruction (contract in Server.h); otherwise the still-joinable network thread directly
// terminates the process. An ASSERT failure midway through a test returns early; RAII ensures stop is always executed
class ServerStopper {
public:
    explicit ServerStopper(Server &server) : server(server) {
    }

    ~ServerStopper() {
        server.stop();
    }

    ServerStopper(const ServerStopper &) = delete;
    ServerStopper &operator=(const ServerStopper &) = delete;

private:
    Server &server;
};

std::shared_ptr<UsbDevice> make_keyboard(StringPool &string_pool) {
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
    mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    return mock_keyboard;
}

// Poll until the condition holds. The "nothing happened" property of rejected connections cannot be awaited as an event; consistently use
// the filter's own invocation count for ordering (the filter is invoked once for every accepted connection)
bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// Bind to a specified local port before connecting to the server: simulate "the client first reserves an ephemeral source port, then
// the server allows only that port". connect_with_retry recreates the socket on retry,
// losing the bound port, so retry is implemented locally here
bool connect_from_port(asio::ip::tcp::socket &client, const asio::ip::tcp::endpoint &server_ep,
                       std::uint16_t local_port) {
    for (int i = 0; i < 200; i++) {
        std::error_code ec;
        client.close();
        client = asio::ip::tcp::socket(client.get_executor());
        // bind requires an already-open socket (asio's connect automatically opens it; bind does not)
        client.open(asio::ip::tcp::v4(), ec);
        if (ec) {
            return false;
        }
        client.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), local_port), ec);
        if (ec) {
            // The port may not yet have been released by the previous round; retry
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        client.connect(server_ep, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// Assert that the server has closed the connection: after the filter handles this connection, reading should immediately return EOF (FIN)
// or a reset error (RST), rather than remain blocked
void expect_closed_by_server(asio::ip::tcp::socket &client) {
    char buf[1] = {};
    asio::error_code ec;
    const std::size_t n = client.read_some(asio::buffer(buf), ec);
    EXPECT_TRUE(ec || n == 0) << "Connection was not closed (read " << n << " bytes without an error)";
}

} // namespace

TEST(TestConnectionFilter, AllowsAllWhenNotSet) {
    // Filter unset: behavior exactly matches that before the filter was introduced (allow all by default)
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    EXPECT_EQ(import_device(client, "1-1"), 0u);
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    EXPECT_EQ(server.get_session_count(), 1u);
    client.close();
}

TEST(TestConnectionFilter, RejectClosesConnectionWithoutSessionOrEvent) {
    // Rejected connection: socket closed, no session established, no on_session_started sent.
    // Order the rejected connection's "nothing happened" property using a subsequent allowed connection + filter invocation count
    // (the accept loop is serial; when the allowed connection is handled, the rejected connection must already have been handled)
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    const std::uint16_t allowed_port = probe_free_port(io);
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        filter_calls.fetch_add(1);
        return ep.port() == allowed_port;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    // Source port not in the allowlist: reject
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));

    // Source port in the allowlist: allow
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    ASSERT_EQ(import_device(allowed, "1-1"), 0u);

    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 2; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "Rejected connections must not send on_session_started";
    EXPECT_EQ(server.get_session_count(), 1u) << "Rejected connections must not establish a session";

    expect_closed_by_server(rejected);
}

TEST(TestConnectionFilter, NonWhitelistedPortIsRejected) {
    // Allow only the specified source port (Android same-host loopback scenario: 127.0.0.1 is shared by all apps,
    // and the server trusts only the port just reserved by the client)—reject every connection outside the allowlist
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    const std::uint16_t allowed_port = probe_free_port(io);
    server.set_connection_filter(
            [allowed_port](const asio::ip::tcp::endpoint &ep) { return ep.port() == allowed_port; });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    // Allowlisted port: allow and complete one import
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    ASSERT_EQ(import_device(allowed, "1-1"), 0u);
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    allowed.close();

    // Port outside the allowlist: reject without affecting cleanup of the existing session
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return server.get_session_count() == 0; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "Connections outside the allowlist must not establish a session";
    expect_closed_by_server(rejected);
}

TEST(TestConnectionFilter, FilterSeesPeerEndpoint) {
    // The peer address received by the filter is the client's source address (this case verifies that the source port matches the bound value)
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<std::uint16_t> seen_port{0};
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        seen_port.store(ep.port());
        filter_calls.fetch_add(1);
        return false; // The argument is this case's only focus; reject all
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    const std::uint16_t local_port = probe_free_port(io);
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_from_port(client, server.endpoint(), local_port));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(seen_port.load(), local_port);
}

TEST(TestConnectionFilter, ThrowingFilterRejectsAndServerStaysUsable) {
    // Treat filter exceptions as "reject" (fail-closed), without killing the accept loop:
    // connections establish normally when true is subsequently returned
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) -> bool {
        // The first two invocations throw exceptions (std::exception branch); allow subsequent ones
        if (filter_calls.fetch_add(1) < 2) {
            throw std::runtime_error("Test exception");
        }
        return true;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket first(io);
    asio::ip::tcp::socket second(io);
    asio::ip::tcp::socket third(io);
    ASSERT_TRUE(connect_with_retry(first, server.endpoint()));
    ASSERT_TRUE(connect_with_retry(second, server.endpoint()));
    ASSERT_TRUE(connect_with_retry(third, server.endpoint()));

    // All three connections were filtered: the two throwing exceptions were rejected; the third was allowed
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 3; }));
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "Connections whose filter throws must not establish a session";
    EXPECT_EQ(server.get_session_count(), 1u);

    // Which connection is allowed depends on accept order; find it by port and verify it works
    const auto peers = observer.peer_snapshot();
    ASSERT_EQ(peers.size(), 1u);
    const std::string peer = peers.front(); // Of the form "127.0.0.1:port"
    const std::uint16_t winner_port = static_cast<std::uint16_t>(std::stoi(peer.substr(peer.rfind(':') + 1)));
    asio::ip::tcp::socket *winner = nullptr;
    for (auto *candidate: {&first, &second, &third}) {
        if (candidate->local_endpoint().port() == winner_port) {
            winner = candidate;
            break;
        }
    }
    ASSERT_NE(winner, nullptr) << "The notified connection must be one of the three clients";
    EXPECT_EQ(import_device(*winner, "1-1"), 0u);
}

TEST(TestConnectionFilter, ManyRejectionsThenAllow) {
    // Multiple consecutive rejected connections do not break the accept loop (also indirectly verifies balanced active_sessions counting:
    // each rejection creates and destroys a Session; a count leak would make stop() hang)
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    const std::uint16_t allowed_port = probe_free_port(io);
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        filter_calls.fetch_add(1);
        return ep.port() == allowed_port;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    std::vector<asio::ip::tcp::socket> rejected;
    for (int i = 0; i < 5; i++) {
        rejected.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(rejected.back(), server.endpoint()));
    }
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 5; }));
    EXPECT_EQ(server.get_session_count(), 0u);

    // All rejected connections are closed; the subsequent allowlisted connection works normally
    for (auto &client: rejected) {
        expect_closed_by_server(client);
    }
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    EXPECT_EQ(import_device(allowed, "1-1"), 0u);
    EXPECT_EQ(server.get_session_count(), 1u);
}

TEST(TestConnectionFilter, PersistsAcrossRestart) {
    // The filter is a Server member: it still applies when starting again after stop, with no need to set it again
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) {
        filter_calls.fetch_add(1);
        return false;
    });

    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket first_round(io);
    ASSERT_TRUE(connect_with_retry(first_round, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(server.get_session_count(), 0u);

    server.stop();
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket second_round(io);
    ASSERT_TRUE(connect_with_retry(second_round, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 2; }));
    EXPECT_EQ(server.get_session_count(), 0u) << "The filter must still apply after restart";
}

TEST(TestConnectionFilter, ClearRestoresAllowAll) {
    // Clearing the filter (passing an empty std::function) restores admission for all
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) {
        filter_calls.fetch_add(1);
        return false;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(server.get_session_count(), 0u);
    server.stop();

    server.set_connection_filter(nullptr);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_with_retry(allowed, server.endpoint()));
    EXPECT_EQ(import_device(allowed, "1-1"), 0u);
    EXPECT_EQ(server.get_session_count(), 1u);
    EXPECT_EQ(filter_calls.load(), 1) << "The filter should no longer be invoked after clearing";
}

TEST(TestConnectionFilter, AcceptsLvalueFilter) {
    // Pass the filter as an lvalue (policies often come from lvalues such as configuration objects): the by-value parameter must accept it,
    // copying without emptying the caller's std::function (otherwise the caller cannot reuse it or inspect it afterward)
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<int> filter_calls{0};
    std::function<bool(const asio::ip::tcp::endpoint &)> deny_all = [&](const asio::ip::tcp::endpoint &) {
        filter_calls.fetch_add(1);
        return false;
    };

    server.set_connection_filter(deny_all);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(server.get_session_count(), 0u);
    EXPECT_TRUE(static_cast<bool>(deny_all)) << "Passing by value must not empty the caller's std::function";

    server.stop();
}
