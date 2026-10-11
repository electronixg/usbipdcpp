#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

// ===== HID keyboard (Boot protocol, no Report ID) =====
// standard 63 byte Boot keyboard report descriptor: 8 byte IN report (modifier + reserved + 6 key)
// + 1 Byte OUT Report (LED)
static const data_type kKeyboardReportDescriptor = {
        0x05, 0x01, // Usage Page (Generic Desktop)
        0x09, 0x06, // Usage (Keyboard)
        0xA1, 0x01, // Collection (Application)
        0x05, 0x07, //   Usage Page (Key Codes)
        0x19, 0xE0, //   Usage Minimum (224 = Left Ctrl)
        0x29, 0xE7, //   Usage Maximum (231 = Right GUI)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x08, //   Report Count (8)
        0x81, 0x02, //   Input (Data, Variable, Absolute) —— 8 modifier-key bits
        0x95, 0x01, //   Report Count (1)
        0x75, 0x08, //   Report Size (8)
        0x81, 0x01, //   Input (Constant) —— reserved bytes
        0x95, 0x05, //   Report Count (5)
        0x75, 0x01, //   Report Size (1)
        0x05, 0x08, //   Usage Page (LEDs)
        0x19, 0x01, //   Usage Minimum (1)
        0x29, 0x05, //   Usage Maximum (5)
        0x91, 0x02, //   Output (Data, Variable, Absolute) —— 5 LEDs
        0x95, 0x01, //   Report Count (1)
        0x75, 0x03, //   Report Size (3)
        0x91, 0x01, //   Output (Constant)
        0x95, 0x06, //   Report Count (6)
        0x75, 0x08, //   Report Size (8)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x65, //   Logical Maximum (101)
        0x05, 0x07, //   Usage Page (Key Codes)
        0x19, 0x00, //   Usage Minimum (0)
        0x29, 0x65, //   Usage Maximum (101)
        0x81, 0x00, //   Input (Data, Array) —— 6 key slots
        0xC0,       // End Collection
};

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_pipe_hid", "USB/IP virtual HID keyboard via generic pipe (read/write)");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    // HID Boot keyboard interface: Interrupt IN endpoint (8 byte report)
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0x03, // HID
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03, // Interrupt
                                    .max_packet_size = 8,
                                    .interval = 10,
                            },
                    }},
            },
    };

    // The pipeline device does not have an interface handler (PipeDeviceHandler automatically pipelines all endpoints), and the interface is manually defined.
    auto device = UsbDevice::make(busid, 0x1234, 0x5691, interfaces, 1, 1, 0, "/usbipdcpp/mock_pipe_hid");

    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    // HID descriptor (0x21): appended after the interface descriptor of the configured descriptor, necessary for driver loading
    pipe->set_class_specific_descriptor({
            0x09, // bLength
            0x21, // bDescriptorType: HID
            0x11,
            0x01, // bcdHID 1.11
            0x00, // bCountryCode
            0x01, // bNumDescriptors
            0x22, // bDescriptorType[0]: Report
            static_cast<std::uint8_t>(kKeyboardReportDescriptor.size()),
            static_cast<std::uint8_t>(kKeyboardReportDescriptor.size() >> 8), // wDescriptorLength[0]
    });
    // GET_DESCRIPTOR(0x22) Return report descriptor
    pipe->set_custom_descriptor(0x22, kKeyboardReportDescriptor);
    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock pipe HID keyboard started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // The business thread uses short timeout polling: the exit sequence must be "the business thread exits first, then server.stop()",
    // Otherwise, when the handler is destroyed with the session, the business thread is still blocked on read/write (use-after-free)
    std::atomic<bool> running{true};

    // read thread: Processes OUT data and non-standard control requests (HID
    // GET_REPORT/SET_REPORT and other class requests will also be returned through read())
    std::thread read_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (!pipe->read(xfer, 200)) {
                continue;
            }
            if (xfer.setup_req.has_value()) {
                // control request (ep==0): class/vendor request. IN direction requires response
                SPDLOG_INFO("received control request req={:02x} type={:02x} length={}",
                            xfer.setup_req->request, xfer.setup_req->request_type, xfer.setup_req->length);
                if (!xfer.setup_req->is_out() && xfer.setup_req->length > 0) {
                    // responds to the current keyboard report (simplified example: GET_REPORT returns an empty report)
                    pipe->write(PipeXfer{.ep = 0, .data = data_type(xfer.setup_req->length, 0)}, 200);
                }
            }
            else if (!xfer.data.empty()) {
                // OUT data: HID keyboard does not have OUT data endpoint, it will be printed as soon as it is received.
                SPDLOG_INFO("Received {} bytes (ep {:02x}); LED/OUT report: {:02x}",
                            xfer.data.size(), xfer.ep, xfer.data[0]);
            }
        }
    });

    // Send thread: Press and release the A key every 2 seconds (demonstrates blocking write)
    std::thread sender_thread([&]() {
        while (running) {
            // Press A (USB HID key code 0x04)
            const data_type pressed = {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
            pipe->write(PipeXfer{.ep = 0x81, .data = pressed}, 200);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Release all buttons
            const data_type released = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            pipe->write(PipeXfer{.ep = 0x81, .data = released}, 200);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    wait_for_exit();

    running = false;
    read_thread.join();
    sender_thread.join();
    server.stop();
    return 0;
}
