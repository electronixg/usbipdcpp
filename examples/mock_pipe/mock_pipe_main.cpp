#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_pipe", "USB/IP virtual generic pipe device (read/write)");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    // vendor class interface: bulk IN + bulk OUT (generic pipe, no class-specific protocol)
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF, // vendor specific
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };

    // The pipeline device does not have an interface handler (PipeDeviceHandler automatically pipelines all endpoints), and the interface is manually defined.
    auto device = UsbDevice::make(busid, 0x1234, 0x5690, interfaces, 1, 1, 0, "/usbipdcpp/mock_pipe");
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);

    // standard request behavior configuration (optional, default behavior: accept existing alt, return errors for the rest):
    // Demonstration rejects interface level SET_FEATURE - This device (vendor pipeline) does not support any interface level
    // feature, replying to an error is clearer than pretending to be successful. Must be set before connecting
    PipeStandardRequestHandler req_handler;
    req_handler.set_feature = [](PipeDeviceHandler &pipe, std::uint16_t feature_selector,
                                 std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    };
    pipe->set_standard_request_handler(std::move(req_handler));

    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock pipe device started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // The business thread uses short timeout polling: the exit sequence must be "the business thread exits first, then server.stop()",
    // Otherwise, when the handler is destroyed with the session, the business thread is still blocked on read/write (use-after-free).
    // read/write returns immediately during normal communication, and timeout polling does not affect use
    std::atomic<bool> running{true};

    // echo thread: The received OUT data is sent back as it is (blocking read)
    std::thread echo_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (pipe->read(xfer, 200)) {
                SPDLOG_INFO("Received {} bytes (ep {:02x}); echoing", xfer.data.size(), xfer.ep);
                pipe->write(PipeXfer{.ep = 0x81, .data = std::move(xfer.data)}, 200);
            }
        }
    });

    // periodic sending thread: sending a count message every 2 seconds (demonstrating blocking write).
    // logs for e2e script to assert transfer activity (the host has no vendor driver, and the data side is verified by python.
    // Here only confirms that the sending thread is alive)
    std::thread sender_thread([&]() {
        std::uint32_t count = 0;
        while (running) {
            auto msg = "message " + std::to_string(count++) + "\r\n";
            SPDLOG_INFO("Periodic send: {} bytes: {}", msg.size(), msg);
            pipe->write(PipeXfer{.ep = 0x81, .data = data_type(msg.begin(), msg.end())}, 200);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    wait_for_exit();

    running = false;
    echo_thread.join();
    sender_thread.join();
    server.stop();
    return 0;
}
