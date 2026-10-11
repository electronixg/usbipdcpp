#include <iostream>

#include "../example_utils.h"
#include "mock_cdc_acm.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_cdc_acm", "USB/IP virtual serial port device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM requires two interfaces: communication interface and data interface (make_interface only returns the descriptor template,
    // device is created and bound to the Mock handler in the example)
    std::vector<UsbInterface> interfaces = {
            CdcAcmCommunicationInterfaceHandler::make_interface(0x83),
            CdcAcmDataInterfaceHandler::make_interface(0x81, 0x02),
    };

    // Create device: IAD composite device needs to declare CDC class at the device level
    auto mock_cdc_acm = UsbDevice::make(busid, 0x1234, 0x5680, std::move(interfaces),
                                        1, 1, static_cast<std::uint8_t>(ClassCode::CDC), "/usbipdcpp/mock_cdc_acm");
    // interfaces are numbered consecutively from 0: fill in interface_number in sequence according to the subscript (bInterfaceNumber of multi-interface devices depends on it)
    mock_cdc_acm->assign_interface_numbers();
    // sets the interface processor (with_handler returns the specific type of shared_ptr, which can be used directly without cast)
    auto comm_handler = mock_cdc_acm->interfaces[0].with_handler<MockCdcAcmCommunicationInterfaceHandler>(string_pool);
    auto data_handler = mock_cdc_acm->interfaces[1].with_handler<MockCdcAcmDataInterfaceHandler>(string_pool);
    mock_cdc_acm->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // associates communication interface and data interface processor (descriptor references data interface number + data plane status query)
    comm_handler->set_data_handler(data_handler.get());
    data_handler->set_comm_handler(comm_handler.get());

    Server server;
    server.add_device(std::move(mock_cdc_acm));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock CDC ACM (virtual serial port) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Then use: screen /dev/ttyACMx or minicom");
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    return 0;
}
