#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

/**
 * @brief TUN/TAP backend: connect the virtual network adapter to the host's real kernel protocol stack (Linux /dev/net/tun)
 *
 * Create the device with IFF_TAP (layer 2, behaves like a real Ethernet adapter): write Ethernet frames from the host (USB client)
 * into the tap device; the kernel processes them as ordinary network-adapter frames (the kernel protocol stack responds to ARP / ICMP / TCP,
 * and can route/forward to real networks); a read thread pushes complete Ethernet frames written back to tap by the kernel to the host through
 * send_to_host. The tap interface is destroyed when fd closes.
 *
 * @note Available only on Linux-kernel platforms (including WSL2 / Android termux / armbian): TUNSETIFF and
 * adapter configuration (IP/netmask/UP) require root privileges. This file is not compiled on Windows/macOS (macOS
 * utun is layer 3 with no public TAP interface; the tuntaposx driver is disabled on newer systems)
 */
class USBIPDCPP_API TunBackend : public NetworkBackend {
public:
    /**
     * @param if_name Interface name (the kernel assigns a number for %d, such as "usbip%d"); after creation,
     *        query the actual name with interface_name()
     * @param ip Device-side IPv4 (4 bytes, such as 192.168.53.1)
     * @param netmask Subnet mask (such as 255.255.255.0)
     * @throws std::runtime_error Failure opening /dev/net/tun or calling ioctl (root privileges /
     *        kernel lacks CONFIG_TUN / invalid interface name)
     */
    TunBackend(std::string if_name, std::array<std::uint8_t, 4> ip, std::array<std::uint8_t, 4> netmask);

    ~TunBackend() override;

    /// Host sends a frame (called by the USB receive-stream thread): write to the tun device for the kernel protocol stack
    void send_frame(const std::uint8_t *data, std::size_t size) override;

    /// Actual interface name created (TUNSETIFF expands %d to a number)
    [[nodiscard]] const std::string &interface_name() const {
        return if_name_;
    }

private:
    /// Blocking read from the tun device: push frames from the kernel protocol stack to the host through send_to_host
    void read_loop();

    int tun_fd_ = -1;
    std::string if_name_;
    std::thread read_thread_;
    std::atomic_bool stop_{false};
};

} // namespace usbipdcpp
