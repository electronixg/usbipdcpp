// ServerObserver notification tests: event timing and arguments, multiple observers, idempotent registration,
// no notifications after unregistration, callback exceptions affecting neither server nor other observers, querying/operating Server in callbacks
// without deadlock (proof of notifications outside locks), observer-count limit
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
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
// Observer recording all events. Callbacks run on session/backend threads, with internal locking
class RecordingObserver : public ServerObserver {
public:
    struct Event {
        std::string kind;
        std::uint64_t session_id = 0;
        std::string text; // started records peer; attached/released record busid
        DeviceReleaseReason reason = DeviceReleaseReason::ClientDisconnected;
    };

    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        record({"started", session_id, peer, DeviceReleaseReason::ClientDisconnected});
    }

    void on_session_ended(std::uint64_t session_id) override {
        record({"ended", session_id, {}, DeviceReleaseReason::ClientDisconnected});
    }

    void on_device_added(const std::string &busid) override {
        record({"added", 0, busid, DeviceReleaseReason::ClientDisconnected});
    }

    void on_device_attached(const std::string &busid) override {
        record({"attached", 0, busid, DeviceReleaseReason::ClientDisconnected});
    }

    void on_device_released(const std::string &busid, DeviceReleaseReason reason) override {
        record({"released", 0, busid, reason});
    }

    std::vector<Event> snapshot() const {
        std::lock_guard lock(mutex);
        return events;
    }

    std::size_t count(const std::string &kind) const {
        std::lock_guard lock(mutex);
        return std::count_if(events.begin(), events.end(), [&](const Event &e) { return e.kind == kind; });
    }

private:
    void record(Event event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    mutable std::mutex mutex;
    std::vector<Event> events;
};

const RecordingObserver::Event *find_event(const std::vector<RecordingObserver::Event> &events,
                                           const std::string &kind) {
    for (const auto &e: events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

std::size_t count_events(const std::vector<RecordingObserver::Event> &events, const std::string &kind,
                         const std::string &text) {
    return std::count_if(events.begin(), events.end(), [&](const RecordingObserver::Event &e) {
        return e.kind == kind && e.text == text;
    });
}

// Poll until observer receives at least expected specified events. For "disconnect immediately after connection"
// scenarios: connection may still be in accept queue (session not created,
// get_session_count is 0); wait_sessions_gone would return early before event emission
bool wait_for_event_count(const RecordingObserver &observer, const std::string &kind, std::size_t expected,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (observer.count(kind) >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return observer.count(kind) >= expected;
}

// Server must already be stopped before destruction (contract in Server.h); otherwise the still-joinable network thread directly
// terminates the process. ASSERT failure midway through test returns early; RAII guarantees stop executes
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

// Observer throwing from every callback (on_session_started throws std::exception,
// on_device_released throws a non-std::exception; cover both catch branches)
class ThrowingObserver : public ServerObserver {
public:
    void on_session_started(std::uint64_t, const std::string &) override {
        started_calls.fetch_add(1);
        throw std::runtime_error("Test exception");
    }

    void on_device_released(const std::string &, DeviceReleaseReason) override {
        released_calls.fetch_add(1);
        throw 42;
    }

    std::atomic<int> started_calls{0};
    std::atomic<int> released_calls{0};
};

// Observer querying Server inside callbacks: notifying while holding locks would necessarily deadlock here
// (devices_mutex / session_list_mutex both nonrecursive); test would fail by timeout
class QueryingObserver : public ServerObserver {
public:
    Server *server = nullptr;
    std::atomic<std::size_t> session_count_seen{0};
    std::atomic<bool> attached_device_bound{false};
    std::atomic<std::size_t> using_count_seen{0};

    void on_session_started(std::uint64_t, const std::string &) override {
        session_count_seen.store(server->get_session_count());
    }

    void on_device_attached(const std::string &busid) override {
        attached_device_bound.store(server->has_bound_device(busid));
        std::shared_lock lock(server->get_devices_mutex());
        using_count_seen.store(server->get_using_devices().size());
    }
};

// Unregister itself on first session-start event: notification iteration uses snapshot taken under lock,
// so modifying registry inside callbacks (including self-unregistration) is safe
class SelfRemovingObserver : public ServerObserver {
public:
    Server *server = nullptr;
    std::atomic<int> started_count{0};

    void on_session_started(std::uint64_t, const std::string &) override {
        started_count.fetch_add(1);
        server->remove_observer(this);
    }
};

// Device-level handler markable as "physically removed"
class ToggleRemovedDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    std::shared_ptr<std::atomic_bool> removed_flag;

    bool is_device_removed() const override {
        return removed_flag && removed_flag->load();
    }
};

// Construct test virtual keyboard; nonempty removed_flag permits device-level handler to be marked "physically removed"
std::shared_ptr<UsbDevice> make_keyboard(StringPool &string_pool,
                                         std::shared_ptr<std::atomic_bool> removed_flag = nullptr,
                                         const std::string &busid = "1-1") {
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
            .busid = busid,
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
    if (removed_flag) {
        auto handler = mock_keyboard->with_handler<ToggleRemovedDeviceHandler>(string_pool);
        handler->removed_flag = removed_flag;
        handler->setup_interface_handlers();
    }
    else {
        mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    }
    return mock_keyboard;
}
} // namespace

TEST(TestServerObserver, SessionAndDeviceEventsOnClientDisconnect) {
    // Full round: connect → import → disconnect. Event order: started first, ended last,
    // attached / released between them (once for successful import and client disconnection respectively)
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    // Wait for events to finish rather than session count: remove_session erases under lock and notifies outside lock;
    // when get_session_count reaches zero, on_session_ended may still be in progress
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    const auto events = observer.snapshot();
    ASSERT_FALSE(events.empty());
    // Observer registered after add_device: no on_device_added; first event is session start
    EXPECT_EQ(events.front().kind, "started");
    EXPECT_NE(events.front().session_id, 0u);
    EXPECT_FALSE(events.front().text.empty()); // peer shaped as "ip:port"

    const auto *attached = find_event(events, "attached");
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(attached->text, "1-1");

    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::ClientDisconnected);

    EXPECT_EQ(events.back().kind, "ended");
    EXPECT_EQ(events.back().session_id, events.front().session_id);

    // Each event exactly once, without duplicates
    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, DeviceReleasedReasonDeviceRemoved) {
    // Device physically unplugged during transfer; release on client disconnection attributed to DeviceRemoved
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_keyboard(string_pool, removed_flag));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    removed_flag->store(true); // Device physically unplugged during transfer
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    const auto events = observer.snapshot();
    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::DeviceRemoved);

    server.stop();
}

TEST(TestServerObserver, DeviceReleasedReasonServerStopped) {
    // Client still connected; stop() directly: device release attributed to ServerStopped
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    server.stop(); // Without closing client, take stop interruption path

    const auto events = observer.snapshot();
    const auto *attached = find_event(events, "attached");
    ASSERT_NE(attached, nullptr);
    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::ServerStopped);

    client.close();
}

TEST(TestServerObserver, MultipleObserversAllNotified) {
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer_a;
    RecordingObserver observer_b;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer_a);
    server.add_observer(&observer_b);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer_a, "ended", 1));
    ASSERT_TRUE(wait_for_event_count(observer_b, "ended", 1));

    for (auto *observer: {&observer_a, &observer_b}) {
        EXPECT_EQ(observer->count("started"), 1u);
        EXPECT_EQ(observer->count("attached"), 1u);
        EXPECT_EQ(observer->count("released"), 1u);
        EXPECT_EQ(observer->count("ended"), 1u);
    }

    server.stop();
}

TEST(TestServerObserver, AddObserverIsIdempotent) {
    // Registering same pointer twice retains only one; each event still notified once
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, RemovedObserverStopsReceiving) {
    // No further events after unregistration; other registered observers unaffected
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer_a;
    RecordingObserver observer_b;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer_a);
    server.add_observer(&observer_b);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    server.remove_observer(&observer_a);
    server.remove_observer(&observer_a); // Repeated unregistration harmless

    client.close();
    ASSERT_TRUE(wait_for_event_count(observer_b, "ended", 1));

    EXPECT_EQ(observer_a.count("started"), 1u); // Received started before unregistration
    EXPECT_EQ(observer_a.count("ended"), 0u);   // No ended received after unregistration
    EXPECT_EQ(observer_a.count("released"), 0u);
    EXPECT_EQ(observer_b.count("ended"), 1u);
    EXPECT_EQ(observer_b.count("released"), 1u);

    server.stop();
}

TEST(TestServerObserver, CallbackExceptionDoesNotAffectServer) {
    // Throwing observer affects neither normal server service nor other observers' notifications
    asio::io_context io;
    StringPool string_pool;
    ThrowingObserver thrower;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&thrower);
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u); // Service unaffected by exception
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_GT(thrower.started_calls.load(), 0);
    EXPECT_GT(thrower.released_calls.load(), 0);
    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    // Service still usable after exception: another full round
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    EXPECT_EQ(import_device(client2, "1-1"), 0u);
    client2.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 2));
    EXPECT_EQ(observer.count("ended"), 2u);

    server.stop();
}

TEST(TestServerObserver, CallbackCanQueryServerWithoutDeadlock) {
    // Notifications outside locks: callbacks may call locking Server interfaces (notification under lock would deadlock)
    asio::io_context io;
    StringPool string_pool;
    QueryingObserver observer;
    Server server;
    ServerStopper stopper(server);
    observer.server = &server;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    EXPECT_EQ(observer.session_count_seen.load(), 1u); // Session already in table at started
    EXPECT_TRUE(observer.attached_device_bound.load());
    EXPECT_EQ(observer.using_count_seen.load(), 1u);

    server.stop();
}

TEST(TestServerObserver, CallbackCanRemoveItself) {
    // Self-unregistration in callback: notification iteration uses snapshot; registry modification does not affect current iteration
    asio::io_context io;
    StringPool string_pool;
    SelfRemovingObserver observer;
    Server server;
    ServerStopper stopper(server);
    observer.server = &server;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    EXPECT_EQ(observer.started_count.load(), 1);

    // Unregistered: no started for second connection
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    client2.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    EXPECT_EQ(observer.started_count.load(), 1);

    server.stop();
}

TEST(TestServerObserver, ObserverLimitIgnoresExtraRegistrations) {
    // Limit 8 (matching Server::MAX_OBSERVERS): excess registrations ignored,
    // existing observers unaffected
    constexpr std::size_t kMaxObservers = 8;
    asio::io_context io;
    StringPool string_pool;
    std::vector<std::unique_ptr<RecordingObserver>> observers;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    for (std::size_t i = 0; i < kMaxObservers + 1; ++i) {
        observers.push_back(std::make_unique<RecordingObserver>());
        server.add_observer(observers.back().get());
    }
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(*observers[0], "ended", 1));

    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        EXPECT_EQ(observers[i]->count("started"), 1u) << "Observer " << i;
    }
    EXPECT_EQ(observers[kMaxObservers]->count("started"), 0u); // Excess ignored

    server.stop();
}

TEST(TestServerObserver, NoDeviceEventsWhenNeverImported) {
    // Connect and disconnect without import: no device events (attached emitted only after actual successful import).
    // FIN disconnection necessarily establishes session (kernel passes socket to accept; server reads EOF and
    // cleans up), so session events necessarily emitted; RST may be discarded by kernel before accept
    // (accept returns connection_aborted; server creates no session); RST branch waits only
    // for possible session cleanup, without asserting session events
    for (bool use_rst: {false, true}) {
        asio::io_context io;
        StringPool string_pool;
        RecordingObserver observer;
        Server server;
        ServerStopper stopper(server);
        server.add_device(make_keyboard(string_pool));
        server.add_observer(&observer);
        ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        if (use_rst) {
            rst_disconnect(client);
        }
        else {
            client.close();
            // Wait for events rather than session count: connection may still be in accept queue
            ASSERT_TRUE(wait_for_event_count(observer, "started", 1));
            ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));
            EXPECT_EQ(observer.count("started"), 1u);
            EXPECT_EQ(observer.count("ended"), 1u);
        }
        ASSERT_TRUE(wait_sessions_gone(server));

        EXPECT_EQ(observer.count("attached"), 0u);
        EXPECT_EQ(observer.count("released"), 0u);

        server.stop();
    }
}

TEST(TestServerObserver, FailedImportEmitsNoDeviceEvents) {
    // Import of nonexistent busid fails (NoDev): no attached; when session disconnects there was no prior
    // import, so no released
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "9-9"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);
    EXPECT_EQ(observer.count("attached"), 0u);
    EXPECT_EQ(observer.count("released"), 0u);

    server.stop();
}

TEST(TestServerObserver, NullObserverIsIgnored) {
    // Null-pointer registration/unregistration harmless, existing observers unaffected
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(nullptr);
    server.remove_observer(nullptr);
    server.add_observer(&observer);
    server.add_observer(nullptr);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, ServerRestartKeepsObservers) {
    // Observers remain registered across start/stop; stopping without imported device emits no device event
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));
    server.stop();
    EXPECT_EQ(observer.count("released"), 1u);

    // Observers still present after restart: second-round events continue arriving
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    ASSERT_EQ(import_device(client2, "1-1"), 0u);
    client2.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 2));

    EXPECT_EQ(observer.count("started"), 2u);
    EXPECT_EQ(observer.count("attached"), 2u);
    EXPECT_EQ(observer.count("released"), 2u);
    EXPECT_EQ(observer.count("ended"), 2u);

    // Stop without any imported device: no release occurred, no device events
    server.stop();
    EXPECT_EQ(observer.count("released"), 2u);
}

TEST(TestServerObserver, ConcurrentSessionsAllNotified) {
    // 3 clients each import one device, operating concurrently: events paired by session/device,
    // session ids distinct, each device attached/released once
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool, nullptr, "1-1"));
    server.add_device(make_keyboard(string_pool, nullptr, "1-2"));
    server.add_device(make_keyboard(string_pool, nullptr, "1-3"));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    std::vector<asio::ip::tcp::socket> clients;
    for (const auto *busid: {"1-1", "1-2", "1-3"}) {
        clients.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(clients.back(), server.endpoint()));
        ASSERT_EQ(import_device(clients.back(), busid), 0u);
    }
    for (auto &client: clients) {
        client.close();
    }
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 3));

    EXPECT_EQ(observer.count("started"), 3u);
    EXPECT_EQ(observer.count("ended"), 3u);
    EXPECT_EQ(observer.count("attached"), 3u);
    EXPECT_EQ(observer.count("released"), 3u);

    std::set<std::uint64_t> session_ids;
    const auto events = observer.snapshot();
    for (const auto &e: events) {
        if (e.kind == "started") {
            session_ids.insert(e.session_id);
        }
    }
    EXPECT_EQ(session_ids.size(), 3u); // Three session ids distinct
    for (const auto *busid: {"1-1", "1-2", "1-3"}) {
        EXPECT_EQ(count_events(events, "attached", busid), 1u) << busid;
        EXPECT_EQ(count_events(events, "released", busid), 1u) << busid;
    }

    server.stop();
}

TEST(TestServerObserver, ReRegisterAfterRemoveWithinLimit) {
    // New registration ignored when full; unregistering one frees slot for successful registration, and unregistered observer
    // receives no further events (replacement at final slot must not lose another observer)
    constexpr std::size_t kMaxObservers = 8;
    asio::io_context io;
    StringPool string_pool;
    std::vector<std::unique_ptr<RecordingObserver>> observers;
    RecordingObserver rejected;
    RecordingObserver newcomer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        observers.push_back(std::make_unique<RecordingObserver>());
        server.add_observer(observers.back().get());
    }
    server.add_observer(&rejected); // Full; ignored
    server.remove_observer(observers[3].get());
    server.add_observer(&newcomer); // Replacement succeeded
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(*observers[0], "ended", 1));

    EXPECT_EQ(newcomer.count("started"), 1u);
    EXPECT_EQ(observers[3]->count("started"), 0u); // Unregistered
    EXPECT_EQ(rejected.count("started"), 0u);      // Ignored while full
    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        if (i == 3) {
            continue;
        }
        EXPECT_EQ(observers[i]->count("started"), 1u) << "Observer " << i;
    }

    server.stop();
}

TEST(TestServerObserver, DisconnectAndStopRaceDoesNotDuplicateEvents) {
    // Stop immediately after client disconnection (session cleanup concurrent with stop): started/attached/
    // released/ended each exactly once (idempotent remove_session + release notified only once)
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close(); // Do not wait for cleanup
    server.stop();  // Stop immediately, concurrent with cleanup path

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);
}

TEST(TestServerObserver, CallbackCanAddObserver) {
    // Register new observer in callback: current iteration uses snapshot and is unaffected; new observer starts
    // receiving notifications with the next event
    class AddingObserver : public ServerObserver {
    public:
        Server *server = nullptr;
        ServerObserver *to_add = nullptr;
        std::atomic<bool> added{false};

        void on_session_started(std::uint64_t, const std::string &) override {
            if (!added.exchange(true)) {
                server->add_observer(to_add);
            }
        }
    };

    asio::io_context io;
    StringPool string_pool;
    AddingObserver adder;
    RecordingObserver latecomer;
    Server server;
    ServerStopper stopper(server);
    adder.server = &server;
    adder.to_add = &latecomer;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&adder);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(latecomer, "ended", 1));

    EXPECT_EQ(latecomer.count("started"), 0u); // Registration occurs after started
    EXPECT_EQ(latecomer.count("attached"), 1u);
    EXPECT_EQ(latecomer.count("released"), 1u);
    EXPECT_EQ(latecomer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, DeviceAddedNotifiedOnAddDevice) {
    // Device enters available list (user add_device), triggering on_device_added with busid argument;
    // notification synchronous (snapshot under lock→synchronous call outside lock); event recorded when add_device returns
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_observer(&observer);

    server.add_device(make_keyboard(string_pool, nullptr, "2-1"));
    server.add_device(make_keyboard(string_pool, nullptr, "2-2"));

    const auto events = observer.snapshot();
    EXPECT_EQ(count_events(events, "added", "2-1"), 1u);
    EXPECT_EQ(count_events(events, "added", "2-2"), 1u);
    // Not started, no clients: no session or device-occupation events expected
    EXPECT_EQ(observer.count("started"), 0u);
    EXPECT_EQ(observer.count("attached"), 0u);
    EXPECT_EQ(observer.count("released"), 0u);
}

TEST(TestServerObserver, DetachAvailableDeviceNotifiesReleasedWithDeviceRemoved) {
    // Device leaves available list (physical unplug/backend unbind uses detach_available_device),
    // emitting on_device_released(DeviceRemoved), same event semantics as "in-use device unplugged" (emitted by
    // release_device): observer only handles "device gone", without distinguishing
    // which list held it. Real trigger path in LibusbServer unplug/unbind handling
    // (handle_device_left / notify_device_removed / try_remove_dead_device /
    // unbind_*), requiring real hardware; here verify Server interface contract itself
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_observer(&observer);

    const auto device = make_keyboard(string_pool);
    EXPECT_EQ(server.add_device(device), AddDeviceStatus::Added);

    // Detach returns detached device (for backend close/unref cleanup, etc.), and emits notification
    const auto detached = server.detach_available_device("1-1");
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(detached, device); // Same object

    const auto events = observer.snapshot();
    ASSERT_EQ(events.size(), 2u); // added + released
    EXPECT_EQ(events.front().kind, "added");
    EXPECT_EQ(events.front().text, "1-1");
    EXPECT_EQ(events.back().kind, "released");
    EXPECT_EQ(events.back().text, "1-1");
    EXPECT_EQ(events.back().reason, DeviceReleaseReason::DeviceRemoved);
    EXPECT_EQ(observer.count("attached"), 0u);

    {
        std::shared_lock lock(server.get_devices_mutex());
        EXPECT_TRUE(server.get_available_devices().empty());
    }

    // Detach nonexistent busid: return empty, no notification
    EXPECT_EQ(server.detach_available_device("9-9"), nullptr);
    EXPECT_EQ(observer.count("released"), 1u);
}

TEST(TestServerObserver, AddDeviceRejectsDuplicateBusid) {
    // Unique busid is an invariant relied on internally by Server: duplicate add rejected without added notification
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_observer(&observer);

    EXPECT_EQ(server.add_device(make_keyboard(string_pool, nullptr, "2-1")), AddDeviceStatus::Added);
    EXPECT_EQ(server.add_device(make_keyboard(string_pool, nullptr, "2-1")), AddDeviceStatus::AlreadyBound);
    EXPECT_EQ(observer.count("added"), 1u) << "Rejected add must not emit notification";

    // When device occupied by session, adding same busid returns InUse
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "2-1"), 0u);
    EXPECT_EQ(server.add_device(make_keyboard(string_pool, nullptr, "2-1")), AddDeviceStatus::InUse);
    EXPECT_EQ(observer.count("added"), 1u);
    client.close();
}