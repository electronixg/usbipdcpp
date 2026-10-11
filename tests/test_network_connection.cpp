// Connection/stop/race tests: a single run rarely exposes problems; many repetitions (e.g.
// ctest --repeat until-fail:2000) are needed to trigger intermittent races and hangs. Stored separately from pure data-structure
// tests (test_network.cpp), making frequent repetition of only these tests convenient:
//   ctest --repeat until-fail:2000 --timeout 120 -R "TestNetworkConnection"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_utils.h"

#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

TEST(TestNetworkConnection,ServerCanRestartAfterStop) {
    // stop() must close acceptor to release the port; otherwise bind to the same port fails on the next start().
    // Port 0 allocated by system: first start updates ep to actual port; second start
    // rebinds the same port to verify release (port 0 avoids probe races during parallel stress tests)
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;

    for (int round = 0; round < 2; round++) {
        ASSERT_FALSE(server.start(ep));
        // Start on port 0; query actual listening endpoint (including system-allocated port) using endpoint()
        auto actual_ep = server.endpoint();

        // acceptor binds synchronously within start(); listening is ready on return; polling is only a defensive
        // wait. Allow 4 seconds: server startup can exceed 1 second in slow builds such as coverage-instrumented builds
        asio::ip::tcp::socket probe_sock(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            probe_sock.connect(actual_ep, ec);
            if (!ec) {
                connected = true;
                break;
            }
            probe_sock.close();
            probe_sock = asio::ip::tcp::socket(io);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);
        // Disconnect actively so server-side session exits promptly
        probe_sock.close();

        server.stop();
    }
}

TEST(TestNetworkConnection,ServerCanStopWithoutConnection) {
    // stop() after start() without any client connection: the blocking accept waiting in accept is
    // interrupted by a wakeup connection; network thread must exit cleanly and permit another start()
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;

    for (int round = 0; round < 3; round++) {
        ASSERT_FALSE(server.start(ep));
        // Stop directly without making any connection
        server.stop();
    }
}

// ---------------------------------------------------------------------------
// Tricky scenarios in start/stop loops (valid usage)
// ---------------------------------------------------------------------------

TEST(TestNetworkConnection,ManyStartStopCycles) {
    // 100 start/stop rounds without clients: acceptor must release port cleanly every round,
    // and network thread must exit cleanly every round, without accumulated leftovers
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    for (int round = 0; round < 100; round++) {
        ASSERT_FALSE(server.start(ep));
        server.stop();
    }
}

TEST(TestNetworkConnection,ServerCanRestartAfterAcceptError) {
#ifdef _WIN32
    // Cannot induce accept errors by exhausting fd on Windows (corresponding WSAEMFILE is uncontrollable); skip
    GTEST_SKIP() << "Cannot induce accept errors by exhausting fd on Windows";
#else
    // Regression for two bugs after accept_loop exits through the error branch on fd exhaustion—
    // - Error branch must close acceptor to release listening port (otherwise occupied until stop())
    // - stop() must clean up normally (network thread already dead; join returns immediately; close idempotent),
    //   and acceptor closed by the error branch must reopen/bind/listen on next start()
    //   (acceptor is a long-lived member, closed on error exit; reinitialization leaves no leftovers)
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    // Port 0 allocated by system; query actual port after start for child-process connect
    const auto port = server.endpoint().port();

    // RAII-wrap occupied fd: ASSERT failure returns directly from test function; vector<int>
    // destruction does not close fd; if a mid-test failure leaves them unreleased, all subsequent tests run with scarce fd
    // (socket creation fails, accept reports EMFILE), producing cascading failures difficult to diagnose.
    // move must empty the source: vector expansion uses move; if both sides hold fd, destruction causes
    // double close, potentially closing a reused fd number of a newly opened socket in a later test
    struct ScopedFd {
        int fd = -1;
        ScopedFd() = default;
        explicit ScopedFd(int f) : fd(f) {
        }
        ScopedFd(const ScopedFd &) = delete;
        ScopedFd &operator=(const ScopedFd &) = delete;
        ScopedFd(ScopedFd &&other) noexcept : fd(other.fd) {
            other.fd = -1;
        }
        ScopedFd &operator=(ScopedFd &&other) noexcept {
            if (this != &other) {
                if (fd >= 0) {
                    ::close(fd);
                }
                fd = other.fd;
                other.fd = -1;
            }
            return *this;
        }
        ~ScopedFd() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };

    // Exhaust process fd: server accept4 then reports EMFILE. This process cannot initiate another
    // connect (WSL tests show connect also allocates fd and immediately yields EMFILE when exhausted), so a forked
    // child initiates the connection: closing inherited fd restores child capacity, leaving connect unaffected
    std::vector<ScopedFd> occupied_fds;
    for (int i = 0; i < 65536; i++) {
        int fd = ::open("/dev/null", O_RDONLY);
        if (fd < 0) {
            break;
        }
        occupied_fds.push_back(ScopedFd{fd});
    }
    ASSERT_FALSE(occupied_fds.empty());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child process: close all fd inherited from parent (except stdin/out/err), restoring its
        // fd capacity; otherwise connect also gives EMFILE. closefrom is undeclared under macOS default feature
        // macros and close_range is Linux-only; consistently use a portable loop.
        // Use only raw socket operations, avoiding asio and gtest (their lock states after fork
        // are unsafe); return result through exit code
        long max_fd = sysconf(_SC_OPEN_MAX);
        for (long fd = 3; fd < max_fd; ++fd) {
            ::close(static_cast<int>(fd));
        }
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            _exit(1);
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int ret = ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        _exit(ret == 0 ? 0 : 1);
    }
    int status = 0;
    ASSERT_GE(waitpid(pid, &status, 0), 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "Child-process connect failed";

    // Keep fd exhausted for a definite duration before release: after child connect returns and before this process releases fd,
    // server must already have executed accept4 (fd still exhausted → EMFILE). Releasing fd first
    // may let accept4 succeed (no EMFILE); continued port listening then causes polling below to time out
    // (reproduced through frequent repetition)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Release early on normal path (ScopedFd destructor covers exceptions), keeping subsequent tests unaffected
    occupied_fds.clear();

    // Poll until server releases listening port: after error branch closes acceptor, port immediately becomes bindable,
    // bindable = network thread exited (also verifies error branch actually closed port)
    bool port_released = false;
    for (int i = 0; i < 200; i++) {
        asio::ip::tcp::acceptor probe(io);
        probe.open(asio::ip::tcp::v4());
        probe.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        std::error_code probe_ec;
        probe.bind(ep, probe_ec);
        if (!probe_ec) {
            port_released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(port_released);

    // stop→start must restore normal operation: network thread already exited on error; stop() join
    // returns immediately, close idempotent; error branch closed acceptor; start() reinitializes it
    server.stop();
    ASSERT_FALSE(server.start(ep));

    // New client connection + normal devlist response = accept loop alive
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    usbipdcpp::error_code send_ec;
    asio::write(client2, asio::buffer(UsbIpCommand::OpReqDevlist{}.to_bytes()), send_ec);
    ASSERT_FALSE(send_ec);
    ASSERT_EQ(usbipdcpp::read_u16(client2), USBIP_VERSION);
    ASSERT_EQ(usbipdcpp::read_u16(client2), OP_REP_DEVLIST);
    client2.close();

    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
#endif
}

TEST(TestNetworkConnection,StopWithSilentClient) {
    // Client connects but sends nothing, remaining silently connected (session blocked reading in parse_op),
    // then stop(): immediately_stop must interrupt pending read; session exits cleanly.
    // Complements interruption in transfer state after device import (ServerCanStopWithImportedDevice)
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

    server.stop(); // Client still silently connected; stop directly

    client.close();
}

TEST(TestNetworkConnection,StopWithMultipleClients) {
    // Multiple clients silently connected (multiple sessions interrupted together); stop() must interrupt concurrently and
    // join all session threads, with none hanging
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    std::vector<asio::ip::tcp::socket> clients;
    for (int i = 0; i < 10; i++) {
        clients.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(clients.back(), server.endpoint()));
    }

    server.stop(); // 10 sessions interrupted together

    for (auto &client: clients) {
        client.close();
    }
}

TEST(TestNetworkConnection,DisconnectRightAfterDevlistRequest) {
    // Client sends devlist request and disconnects immediately without awaiting reply: server may discover disconnection while collecting device
    // list or writing reply; session must exit cleanly without crashing
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    for (int i = 0; i < 5; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        usbipdcpp::error_code send_ec;
        asio::write(client, asio::buffer(UsbIpCommand::OpReqDevlist{}.to_bytes()), send_ec);
        ASSERT_FALSE(send_ec);
        if (i % 2 == 0) {
            rst_disconnect(client); // RST directly without reading reply
        }
        else {
            client.close(); // FIN directly without reading reply
        }
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkConnection,StopDuringClientConnectRace) {
    // stop() competes with concurrent client connection: client thread repeatedly attempts connection (fails while server
    // is stopped); main thread repeatedly starts/stops. A connection may just have been accepted at stop,
    // with session just created; no round may crash or hang
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    std::atomic_bool stop_flag = false;
    // Query actual port (started on system-allocated port 0) using endpoint() after start returns,
    // passing it to client thread via atomic variable; directly reading ep/endpoint() on client thread races with
    // writeback during start (observed under TSan)
    std::atomic<std::uint16_t> target_port = 0;

    std::thread client_thread([&]() {
        while (!stop_flag) {
            auto port = target_port.load();
            if (port != 0) {
                asio::ip::tcp::socket client(io);
                std::error_code ec;
                client.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
                if (!ec) {
                    client.close(); // Disconnect immediately after connection, simulating a short-lived connection
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int round = 0; round < 50; round++) {
        ASSERT_FALSE(server.start(ep));
        target_port.store(server.endpoint().port());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.stop();
        target_port.store(0);
    }

    stop_flag = true;
    client_thread.join();
}

TEST(TestNetworkConnection,ManyQuickConnections) {
    // Clients rapidly connect and disconnect in various ways: graceful disconnection (FIN), RST reset, sending garbage
    // then disconnecting. Server must clean up everything (each session exits), then stop() normally
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    for (int i = 0; i < 50; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        switch (i % 3) {
            case 0:
                client.close(); // Graceful disconnection (FIN)
                break;
            case 1:
                rst_disconnect(client); // Abnormal disconnection (RST)
                break;
            case 2: {
                // Send garbage data, then disconnect: server either fails parsing or has its read interrupted
                const std::array<std::uint8_t, 8> garbage = {0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01};
                asio::write(client, asio::buffer(garbage));
                client.close();
                break;
            }
        }
    }

    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}
