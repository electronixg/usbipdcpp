#include <iostream>

#include "../example_utils.h"
#include "mock_cdc_throttle.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

using namespace usbipdcpp;

/**
 * Example of rate-limited serial port: only a fixed number of bytes can be received within a fixed window, and the device will suspend reception after exceeding the limit (OUT NAK
 * back pressure), it will automatically recover after a period of time. out_channel that demonstrates CdcAcmDataInterfaceHandler
 * suspend mechanism (on_data_received returns false → the request is suspended → the host NAK stops issuance).
 *
 * Usage:
 *   ./mock_cdc_throttle --limit-bytes 128 --window-sec 3
 * The host side can write at most 128 bytes in every 3 second window; if too much is written, the device NAK 3 seconds and then recovers.
 */
int main(int argc, char **argv) {
    auto opts = make_example_options("mock_cdc_throttle", "USB/IP virtual serial port with OUT NAK throttling");
    opts.add_options()("l,limit-bytes", "The upper limit of bytes received per window (this window will suspend reception after reaching it)",
                       cxxopts::value<std::uint32_t>()->default_value("64"))
            ("w,window-sec", "Window duration (seconds): After the rate limit is reached, reception will be suspended for this long and then resumed.",
             cxxopts::value<std::uint32_t>()->default_value("5"));
    auto result = parse_example_args(opts, argc, argv);

    const auto port = result["port"].as<std::uint16_t>();
    const auto busid = result["busid"].as<std::string>();
    const auto limit_bytes = result["limit-bytes"].as<std::uint32_t>();
    const auto window_ms = result["window-sec"].as<std::uint32_t>() * 1000u;

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM requires two interfaces: communication interface and data interface (descriptor consistent with mock_cdc_acm).
    // Use the rate-limiting factory for the data interface: receive at most limit_bytes within a window, then NAK for window_ms
    std::vector<UsbInterface> interfaces = {
            CdcAcmCommunicationInterfaceHandler::make_interface(0x83),
            ThrottleCdcAcmDataInterfaceHandler::make_interface(0x81, 0x02),
    };

    // IAD composite device needs to declare CDC class at the device level
    auto device = UsbDevice::make(busid, 0x1234, 0x5681, std::move(interfaces),
                                  1, 1, static_cast<std::uint8_t>(ClassCode::CDC), "/usbipdcpp/mock_cdc_throttle");
    // interfaces are numbered consecutively from 0: fill in interface_number in sequence according to the subscript (bInterfaceNumber of multi-interface devices depends on it)
    device->assign_interface_numbers();
    device->interfaces[0].with_handler<CdcAcmCommunicationInterfaceHandler>(string_pool);
    device->interfaces[1].with_handler<ThrottleCdcAcmDataInterfaceHandler>(string_pool, limit_bytes, window_ms);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // associated communication interface and data interface processor
    auto &comm_handler =
            *std::static_pointer_cast<CdcAcmCommunicationInterfaceHandler>(device->interfaces[0].handler);
    auto &data_handler =
            *std::static_pointer_cast<ThrottleCdcAcmDataInterfaceHandler>(device->interfaces[1].handler);
    comm_handler.set_data_handler(&data_handler);
    data_handler.set_comm_handler(&comm_handler);

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Rate-limited serial port started: port {}, busid {}, each {} ms window accepts at most {} bytes",
                port, busid, window_ms, limit_bytes);
    SPDLOG_INFO("connection: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("The device counts received '1' characters and sends the count back to the host every {} ms as a number followed by \\n", window_ms / 2);
    SPDLOG_INFO("After the host continues to write more than {} bytes, the device pauses receiving {} ms (OUT NAK) and then automatically resumes",
                limit_bytes, window_ms);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();
    server.stop();
    return 0;
}