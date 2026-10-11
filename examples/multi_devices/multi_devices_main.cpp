#include <iostream>

#include "../example_utils.h"
#include "device_factory.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/utils/StringPool.h"

int main(int argc, char **argv) {
    auto opts = make_example_options("multi_devices", "USB/IP multi-device server");
    opts.add_options()("n,count", "Number of virtual devices", cxxopts::value<int>()->default_value("10"));
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto count = result["count"].as<int>();

    // Set log level
    spdlog::set_level(spdlog::level::debug);

    SPDLOG_INFO("Starting multi-device USB/IP server");

    // Create a string pool
    usbipdcpp::StringPool string_pool;

    // Create virtual device
    auto devices = DeviceFactory::create_devices(count, string_pool);

    // Create server
    usbipdcpp::Server server;

    // Add all devices to the server
    for (auto &device: devices) {
        server.add_device(std::move(device));
    }

    // Set listening endpoint
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    // Start the server
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Server started on port {} with {} devices", port, devices.size());
    SPDLOG_INFO("Use 'usbip list -r localhost --tcp-port {}' to list devices", port);
    SPDLOG_INFO("Use 'usbip attach -r localhost --tcp-port {} -b 1-X' to attach a device", port);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // Print all bound devices
    server.print_bound_devices();

    wait_for_exit();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    SPDLOG_INFO("Server stopped");
    return 0;
}
