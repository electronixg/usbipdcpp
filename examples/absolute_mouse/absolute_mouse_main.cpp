/**
 * @file absolute_mouse_main.cpp
 * @brief Absolute coordinate mouse virtual device example
 */

#include <atomic>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/AbsoluteMouseHandler.h"

using namespace usbipdcpp;

void print_usage() {
    std::cout << "\n command list (screen coordinates):\n"
              << "  p - Print the current button status \n"
              << "  pos <x y> - Set the screen coordinate position \n"
              << "  1 - Move to the center of the screen \n"
              << "  2 - Move to upper left corner \n"
              << "  3 - Move to the lower right corner \n"
              << "  6 - Left click \n"
              << "  7 - Right click \n"
              << "  8 - Double click \n"
              << "  9 - Smooth movement (upper left corner → center, 1 second) \n"
              << "  H - Human-like movement (upper left corner → center) \n"
              << "  D - Drag (center→lower right corner)\n"
              << "  hd x1 y1 x2 y2 - Human-like dragging (starting point→end point)\n"
              << "  raw <x y> - Set HID raw coordinates (0-32767)\n"
              << "  screen W H - Set screen size \n"
              << "  bounds x1 y1 x2 y2 - Set screen boundaries \n"
              << "  q - Exit \n"
              << "  h - Show help \n"
              << std::endl;
}

int main(int argc, char **argv) {
    auto opts = make_example_options("absolute_mouse", "USB/IP absolute mouse device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    auto mouse_device = UsbDevice::make(busid, 0x1234, 0x5680,
                                        {AbsoluteMouseHandler::make_interface(0x81)},
                                        1, 1, 0, "/usbipdcpp/absolute_mouse");
    mouse_device->interfaces[0].with_handler<AbsoluteMouseHandler>(string_pool);
    mouse_device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    auto mouse = std::static_pointer_cast<AbsoluteMouseHandler>(mouse_device->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mouse_device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Absolute mouse started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);

    // Initial position: Center of screen
    int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
    int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;
    mouse->set_position(cx, cy);

    print_usage();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        // Split command by spaces
        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string part;
        while (iss >> part) {
            parts.push_back(part);
        }

        if (parts.empty())
            continue;

        const std::string &cmd = parts[0];
        int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
        int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;

        if (cmd == "p") {
            auto state = mouse->get_button_state();
            std::cout << "button status: left button =" << (state.left_button ? "pressed" : "released")
                      << " right button=" << (state.right_button ? "pressed" : "released")
                      << " middle button=" << (state.middle_button ? "pressed" : "released")
                      << " wheel =" << static_cast<int>(state.wheel) << "\n"
                      << "screen range: (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ") - ("
                      << mouse->get_screen_x2() << ", " << mouse->get_screen_y2() << ")\n"
                      << "screen size: " << mouse->get_screen_width() << "x" << mouse->get_screen_height() << std::endl;
        }
        else if (cmd == "pos" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position(x, y);
            std::cout << "Move to screen coordinates: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "1") {
            std::cout << "Move to the center of the screen (" << cx << ", " << cy << ")" << std::endl;
            mouse->set_position(cx, cy);
        }
        else if (cmd == "2") {
            std::cout << "Move to the upper left corner (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ")"
                      << std::endl;
            mouse->set_position(mouse->get_screen_x1() + 1, mouse->get_screen_y1() + 1);
        }
        else if (cmd == "3") {
            std::cout << "Move to the lower right corner (" << mouse->get_screen_x2() - 1 << ", " << mouse->get_screen_y2() - 1 << ")"
                      << std::endl;
            mouse->set_position(mouse->get_screen_x2() - 1, mouse->get_screen_y2() - 1);
        }
        else if (cmd == "6") {
            std::cout << "Left click (" << cx << ", " << cy << ")" << std::endl;
            mouse->left_click(cx, cy);
        }
        else if (cmd == "7") {
            std::cout << "Right click (" << cx << ", " << cy << ")" << std::endl;
            mouse->right_click(cx, cy);
        }
        else if (cmd == "8") {
            std::cout << "Double click (" << cx << ", " << cy << ")" << std::endl;
            mouse->double_click(cx, cy);
        }
        else if (cmd == "9") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "smooth movement (" << x1 << ", " << y1 << ") → (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->move(x1, y1, cx, cy, 1000);
        }
        else if (cmd == "H") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "Human-like movement (" << x1 << ", " << y1 << ") → (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->humanized_move(x1, y1, cx, cy, 1500);
        }
        else if (cmd == "D") {
            int x2 = mouse->get_screen_x2() - 1;
            int y2 = mouse->get_screen_y2() - 1;
            std::cout << "drag (" << cx << ", " << cy << ") → (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->drag(cx, cy, x2, y2, 1000);
        }
        else if (cmd == "hd" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            std::cout << "Human-like dragging (" << x1 << ", " << y1 << ") → (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->humanized_drag(x1, y1, x2, y2, 1500);
        }
        else if (cmd == "raw" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position_raw(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
            std::cout << "HID coordinates: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "screen" && parts.size() >= 3) {
            int w = std::stoi(parts[1]);
            int h = std::stoi(parts[2]);
            mouse->set_screen_size(w, h);
            std::cout << "screen size is set to: " << w << "x" << h << std::endl;
        }
        else if (cmd == "bounds" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            mouse->set_screen_bounds(x1, y1, x2, y2);
            std::cout << "The screen boundary is set to: (" << x1 << ", " << y1 << ") - (" << x2 << ", " << y2 << ")" << std::endl;
        }
        else if (cmd == "q") {
            std::cout << "Exit..." << std::endl;
            server.stop();
            return 0;
        }
        else if (cmd == "h") {
            print_usage();
        }
        else {
            std::cout << "Unknown command: " << cmd << std::endl;
        }
    }

    server.stop();
    return 0;
}
