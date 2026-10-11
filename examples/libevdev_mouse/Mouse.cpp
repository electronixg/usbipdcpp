#include "Mouse.h"

#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <iostream>

using namespace usbipdcpp::umouse;

std::vector<MouseDescript> usbipdcpp::umouse::findAllMouses() {
    std::vector<MouseDescript> result;

    std::filesystem::path root = "/dev/input";
    for (auto &entry: std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() && entry.path().string().contains("event")) {
            auto &full_path = entry.path();
            int fd = open(full_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                libevdev *dev = nullptr;
                if (libevdev_new_from_fd(fd, &dev) == 0) {
                    // Check whether the device is a mouse
                    if (libevdev_has_event_type(dev, EV_REL) &&
                        (libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) ||
                         libevdev_has_event_code(dev, EV_KEY, BTN_RIGHT))) {
                        auto name = libevdev_get_name(dev);
                        result.emplace_back(name, entry.path());
                    }
                    libevdev_free(dev);
                    close(fd);
                }
                else {
                    throw std::system_error(errno, std::system_category(),
                                            std::format("libevdev cannot open {}", full_path.string()));
                }

            }
            else {
                throw std::system_error(errno, std::system_category(),
                                        std::format("open {} error", full_path.string()));
            }
        }
    }
    return result;
}


MouseDevice usbipdcpp::umouse::openMouse(const std::filesystem::path &path) {
    MouseDevice mouse_device;
    // Open device file
    mouse_device.fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (mouse_device.fd < 0) {
        throw std::system_error(errno, std::system_category(),
                                std::format("open {} error", path.string()));
    }

    // Initialize libevdev context
    int rc = libevdev_new_from_fd(mouse_device.fd, &mouse_device.dev);
    if (rc < 0) {
        close(mouse_device.fd);
        throw std::system_error(errno, std::system_category(),
                                std::format("libevdev cannot open {}", path.string()));
    }

    mouse_device.path = path;

    std::cout << "successfully opened the mouse device:" << std::endl;
    std::cout << std::format("\t Name: {}", libevdev_get_name(mouse_device.dev)) << std::endl;;
    std::cout << std::format("\t Path: {}", path.string()) << std::endl;;
    std::cout << std::format("\tManufacturer: {:4x}", libevdev_get_id_vendor(mouse_device.dev)) << std::endl;;
    std::cout << std::format("\t Product: {:4x}", libevdev_get_id_product(mouse_device.dev)) << std::endl;;


    return mouse_device;
}

void usbipdcpp::umouse::closeMouse(MouseDevice &device) {
    if (device.dev) {
        libevdev_free(device.dev);
        device.dev = nullptr;
    }
    if (device.fd >= 0) {
        close(device.fd);
        device.fd = -1;
    }
}
