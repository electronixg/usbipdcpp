#include "libevdev_mouse.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include "../example_utils.h"
#include <iostream>
#include <mutex>
#include <poll.h>
#include <thread>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/devices/RelativeMouseHandler.h"

#include <libevdev/libevdev.h>

#include "Mouse.h"

using namespace usbipdcpp;
using namespace usbipdcpp::umouse;

static std::atomic_bool should_exit{false};

void signal_handler(int sig) {
    should_exit = true;
}

int main(int argc, char **argv) {
    auto opts = make_example_options("libevdev_mouse", "USB/IP libevdev mouse passthrough");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    // Set signal processing
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto mouses = findAllMouses();

    if (mouses.size() > 0) {
        std::cout << std::format("The current system has {} available mouse devices.", mouses.size()) << std::endl;
        for (std::size_t i = 0; i < mouses.size(); i++) {
            std::cout << std::format("No. {} mouse device: {}", i, mouses[i].name) << std::endl;
            std::cout << std::format("Path: {}", mouses[i].path.string()) << std::endl;
            std::cout << std::endl;
        }
        std::cout << std::format("Please enter the mouse device number you want to use:");

        std::size_t index;
        while (std::cin >> index) {
            if (index < mouses.size()) {
                break;
            }
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << std::format("Invalid device number; please enter it again.") << std::endl;
        }
        MouseDevice opened_mouse;

        try {
            opened_mouse = openMouse(mouses[index].path);
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }

        spdlog::set_level(spdlog::level::trace);

        StringPool string_pool;

        // The  interface description is the same as that of the relative mouse (HID 03/00/00 + interrupt IN 8/10), reusing its factory-built interface.
        // then overrides the LibevdevMouseInterfaceHandler for this example (translates libevdev events into mouse reports)
        std::vector<UsbInterface> interfaces = {
                RelativeMouseHandler::make_interface(0x81),
        };

        auto libevdev_mouse = UsbDevice::make(busid, 0x1234, 0x5678, interfaces, 1, 1, 0, "/usbipdcpp/libevdev_mouse",
                                          UsbSpeed::Low, 0xabcd);
        libevdev_mouse->interfaces[0].with_handler<LibevdevMouseInterfaceHandler>(string_pool);
        auto device_handler = libevdev_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool);
        device_handler->setup_interface_handlers();

        LibevdevMouseInterfaceHandler &mouse_interface_handler =
                *std::static_pointer_cast<LibevdevMouseInterfaceHandler>(libevdev_mouse->interfaces[0].handler);

        Server server;

        asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

        server.add_device(std::move(libevdev_mouse));

        if (auto ec = server.start(endpoint); ec) {
            SPDLOG_ERROR("Server startup failed: {}", ec.message());
            return 1;
        }

        SPDLOG_INFO("Libevdev mouse started on port {}, busid {}", port, busid);
        SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);

        struct input_event ev;
        int rc;

        std::cout << "Reading mouse events. Press Ctrl+C to exit..." << std::endl;
        std::cout << "-------- Start of Event Group --------" << std::endl;

        // Use poll to monitor file descriptor
        struct pollfd fds[1];
        fds[0].fd = opened_mouse.fd;
        fds[0].events = POLLIN;

        while (!should_exit) {
            // waiting event (1000 millisecond timeout)
            int ret = poll(fds, 1, 1000);

            if (ret < 0) {
                SPDLOG_ERROR("poll error: {}", std::generic_category().message(errno));
                break;
            }

            if (ret == 0) {
                // Timeout - No events
                continue;
            }


            // Read all available events
            while ((rc = libevdev_next_event(opened_mouse.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                // std::cout<<"Attempt to acquire lock"<<std::endl;
                std::lock_guard lock(mouse_interface_handler.state_mutex);
                // std::cout<<"Get lock"<<std::endl;
                switch (ev.type) {
                    case EV_REL:
                        // mouse movement event
                        switch (ev.code) {
                            case REL_X: {
                                mouse_interface_handler.current_state.move_horizontal = std::clamp(
                                        mouse_interface_handler.current_state.move_horizontal + ev.value, -127, 127);

                                std::cout << std::format("Mouse moved: X={}", ev.value) << std::endl;
                                break;
                            }

                            case REL_Y: {
                                mouse_interface_handler.current_state.move_vertical = std::clamp(
                                        mouse_interface_handler.current_state.move_vertical + ev.value, -127, 127);

                                std::cout << std::format("Mouse moved: Y={}", ev.value) << std::endl;
                                break;
                            }

                            case REL_WHEEL:
                                // mouse_interface_handler.wheel_vertical = ev.value;
                                std::cout << std::format("Mouse wheel: Vertical Ident={}", ev.value) << std::endl;
                                break;
                            case REL_HWHEEL:
                                std::cout << std::format("Mouse wheel: Horizontal Ident={}", ev.value) << std::endl;
                                break;
                            case REL_WHEEL_HI_RES: {
                                auto resized = ev.value / 120;
                                mouse_interface_handler.current_state.wheel_vertical = std::clamp(
                                        mouse_interface_handler.current_state.wheel_vertical + resized, -127, 127);

                                std::cout << std::format("Mouse wheel high resolution: Vertical={}", ev.value)
                                          << std::endl;
                                break;
                            }
                            case REL_HWHEEL_HI_RES:
                                std::cout << std::format("Mouse wheel high resolution: Horizontal={}", ev.value)
                                          << std::endl;
                                break;
                            default:
                                std::cout << std::format("Unknown relative event: code={}, value={}", ev.code, ev.value)
                                          << std::endl;
                        }
                        break;

                    case EV_KEY:
                        // mouse button event
                        switch (ev.code) {
                            case BTN_LEFT:
                                mouse_interface_handler.current_state.left_pressed = ev.value;
                                std::cout << std::format("Left button: {}", ev.value ? "PRESSED" : "RELEASED")
                                          << std::endl;
                                break;
                            case BTN_RIGHT:
                                mouse_interface_handler.current_state.right_pressed = ev.value;
                                std::cout << std::format("Right button: {}", ev.value ? "PRESSED" : "RELEASED")
                                          << std::endl;
                                break;
                            case BTN_MIDDLE:
                                mouse_interface_handler.current_state.middle_pressed = ev.value;
                                std::cout << std::format("Middle button: {}", ev.value ? "PRESSED" : "RELEASED")
                                          << std::endl;
                                break;
                            case BTN_SIDE:
                                mouse_interface_handler.current_state.side_pressed = ev.value;
                                std::cout << std::format("Side button: {}", ev.value ? "PRESSED" : "RELEASED")
                                          << std::endl;
                                break;
                            case BTN_EXTRA:
                                mouse_interface_handler.current_state.extra_pressed = ev.value;
                                std::cout << std::format("Extra button: {}", ev.value ? "PRESSED" : "RELEASED")
                                          << std::endl;
                                break;
                            default:
                                std::cout << std::format("Unknown key event: code={}, value={}", ev.code, ev.value)
                                          << std::endl;
                        }
                        break;

                    case EV_SYN:
                        // Synchronization event - marks the end of an event group
                        if (ev.code == SYN_REPORT) {
                            std::cout << "-------- End of Event Group --------" << std::endl;
                        }
                        break;

                    default:
                        std::cout << std::format("Unknown event type: {}, code={}, value={}", ev.type, ev.code,
                                                 ev.value)
                                  << std::endl;
                }
                mouse_interface_handler.state_cv.notify_all();
            }

            if (rc != -EAGAIN) {
                SPDLOG_ERROR("error when reading event: {}", std::generic_category().message(errno));
                break;
            }
        }

        SPDLOG_INFO("Exiting...");

        closeMouse(opened_mouse);

        server.stop();

        SPDLOG_INFO("Exited");
    }
    else {
        std::cout << "! No mouse device available" << std::endl;
    }


    return 0;
}
