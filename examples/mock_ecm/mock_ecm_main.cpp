#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/EcmVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/EthernetEchoBackend.h"
// TunBackend Only Linux kernel provides /dev/net/tun (including Android/termux), Windows/macOS does not compile
#if defined(__linux__) || defined(__ANDROID__)
#include "usbipdcpp/virtual_device/network_backends/TunBackend.h"
#endif

using namespace usbipdcpp;

namespace {

/// Parse "02:10:83:00:00:01" form MAC address
std::array<std::uint8_t, 6> parse_mac(const std::string &s) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 6> mac{};
    if (s.size() != 17) {
        throw std::runtime_error("MAC address format should be 02:10:83:00:00:01");
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_val(s[i * 3]);
        int lo = hex_val(s[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[i * 3 + 2] != ':')) {
            throw std::runtime_error("MAC address format should be 02:10:83:00:00:01");
        }
        mac[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return mac;
}

/// Parse "192.168.53.1" form IPv4 address
std::array<std::uint8_t, 4> parse_ipv4(const std::string &s) {
    std::array<std::uint8_t, 4> ip{};
    std::size_t pos = 0;
    for (int i = 0; i < 4; i++) {
        int v = 0;
        auto [ptr, ec] = std::from_chars(s.data() + pos, s.data() + s.size(), v);
        if (ec != std::errc() || v < 0 || v > 255 || (i < 3 && (ptr == s.data() + s.size() || *ptr != '.'))) {
            throw std::runtime_error("IPv4 address format should be 192.168.53.1");
        }
        ip[i] = static_cast<std::uint8_t>(v);
        pos = static_cast<std::size_t>(ptr - s.data()) + (i < 3 ? 1 : 0);
    }
    if (pos != s.size()) {
        throw std::runtime_error("IPv4 address format should be 192.168.53.1");
    }
    return ip;
}

} // namespace

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_ecm", "USB/IP virtual ethernet adapter (CDC ECM)");
    opts.add_options()("mac", "Device MAC address", cxxopts::value<std::string>()->default_value("02:10:83:00:00:01"))
            ("ip", "Device-side IPv4 address", cxxopts::value<std::string>()->default_value("192.168.53.1"))
            ("tcp-port", "Device-side TCP echo port", cxxopts::value<std::uint16_t>()->default_value("5000"))
#if defined(__linux__) || defined(__ANDROID__)
            // /dev/net/tun (TunBackend) is only available on Linux/Android, and other platforms do not register this option.
            ("tun", "Use Linux TUN backend instead of user-space echo: route the virtual NIC into this host's kernel "
                    "network stack (needs root); value = interface name (e.g. usbip%d)",
             cxxopts::value<std::string>())
#endif
            ;
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto mac = parse_mac(result["mac"].as<std::string>());
    auto ip = parse_ipv4(result["ip"].as<std::string>());
    auto tcp_port = result["tcp-port"].as<std::uint16_t>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // backend: default echo mode pure user mode response ARP/ICMP/TCP (no root required); --tun mode
    // virtual network card is connected to the local kernel protocol stack (root required, Linux only). handler holds its pointer,
    // must live longer than the device
    std::unique_ptr<NetworkBackend> backend;
#if defined(__linux__) || defined(__ANDROID__)
    if (result.count("tun")) {
        auto tun_name = result["tun"].as<std::string>();
        auto tun = std::make_unique<TunBackend>(tun_name, ip, std::array<std::uint8_t, 4>{255, 255, 255, 0});
        SPDLOG_INFO("TUN backend: interface '{}', device IP {}.{}.{}.{} (ARP/ICMP/TCP answered by kernel stack)",
                    tun->interface_name(), ip[0], ip[1], ip[2], ip[3]);
        backend = std::move(tun);
    }
    else
#endif
    {
        backend = std::make_unique<EthernetEchoBackend>(mac, ip, tcp_port);
    }

    // ECM dual interface: communication interface (interrupt IN notification) + data interface (alt0 empty / alt1 double bulk)
    std::vector<UsbInterface> interfaces = {
            EcmCommunicationInterfaceHandler::make_interface(0x83),
            EcmDataInterfaceHandler::make_interface(0x81, 0x02),
    };

    // IAD composite device needs to declare the CDC class at the device level (aligned with the usage of mock_cdc_acm)
    auto device = UsbDevice::make(busid, 0x1234, 0x56E1, std::move(interfaces),
                                  1, 1, static_cast<std::uint8_t>(ClassCode::CDC), "/usbipdcpp/mock_ecm");
    // interfaces are numbered consecutively from 0: fill in interface_number in sequence according to the subscript (bInterfaceNumber of multi-interface devices depends on it)
    device->assign_interface_numbers();
    auto comm = device->interfaces[0].with_handler<EcmCommunicationInterfaceHandler>(string_pool, mac);
    auto data = device->interfaces[1].with_handler<EcmDataInterfaceHandler>(string_pool, backend.get());
    // The Union of  communication interface descriptor refers to the data interface number (bSlaveInterface0), which is associated during assembly
    comm->set_data_handler(data.get());
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock ECM (virtual ethernet adapter) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Device MAC: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}, device IP: {}.{}.{}.{}", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5], ip[0], ip[1], ip[2], ip[3]);
    if (result.count("tun")) {
        SPDLOG_INFO("Device-side services: provided by this host's kernel network stack (via TUN)");
    }
    else {
        SPDLOG_INFO("Device-side services: ping (ICMP echo), TCP echo on port {}", tcp_port);
    }
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Then on host: ip addr add {}.{}.{}.2/24 dev usbX, ping {}.{}.{}.{}, nc {}.{}.{}.{} {}",
                ip[0], ip[1], ip[2], ip[0], ip[1], ip[2], ip[3], ip[0], ip[1], ip[2], ip[3], tcp_port);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // frame statistics thread: print sending and receiving statistics every 5 seconds
    std::atomic<bool> running{true};
    std::thread stat_thread([&]() {
        auto last_tx = backend->tx_frames();
        auto last_rx = backend->rx_frames();
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            auto tx = backend->tx_frames();
            auto rx = backend->rx_frames();
            SPDLOG_INFO("Frames - rx: {} (+{}), tx: {} (+{})", rx, rx - last_rx, tx, tx - last_tx);
            last_tx = tx;
            last_rx = rx;
        }
    });

    wait_for_exit();

    running = false;
    stat_thread.join();
    server.stop();

    SPDLOG_INFO("Mock ECM stopped. Received {} frames ({} bytes), sent {} frames ({} bytes)", backend->rx_frames(),
                backend->rx_bytes(), backend->tx_frames(), backend->tx_bytes());
    return 0;
}
