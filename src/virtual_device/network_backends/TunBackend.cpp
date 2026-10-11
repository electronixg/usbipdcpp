#include "usbipdcpp/virtual_device/network_backends/TunBackend.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if.h>
#include <linux/if_tun.h>

namespace usbipdcpp {

namespace {
// Frame buffer limit: standard MTU 1500 + Ethernet header, with margin
constexpr std::size_t TUN_MAX_FRAME = 2048;
} // namespace

TunBackend::TunBackend(std::string if_name, std::array<std::uint8_t, 4> ip,
                       std::array<std::uint8_t, 4> netmask) :
    if_name_(std::move(if_name)) {
    // Open tun device: TUNSETIFF binds fd to a newly created tun interface, destroyed when fd closes
    tun_fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (tun_fd_ < 0) {
        throw std::runtime_error("Failed to open /dev/net/tun (root required? kernel lacks CONFIG_TUN?):" +
                                 std::string(std::strerror(errno)));
    }
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
    // Use IFF_TAP (layer-2 device) rather than IFF_TUN: virtual network adapter data consists of complete Ethernet frames
    // (such as ARP requests from host cdc_ether); IFF_TUN is a layer-3 point-to-point device (NOARP),
    // so the kernel directly discards written ARP frames when checking IP version (cross-machine ping failed in tests)
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI; // Ethernet frames (without the 4-byte PI header)
    if (::ioctl(tun_fd_, TUNSETIFF, &ifr) < 0) {
        int err = errno;
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("TUNSETIFF failed:" + std::string(std::strerror(err)));
    }
    if_name_ = ifr.ifr_name; // If containing %d, the kernel fills in the actual interface name

    // Configure IP/netmask and set UP through AF_INET socket ioctls (same mechanism as the ip command)
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        int err = errno;
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("Failed to create configuration socket:" + std::string(std::strerror(err)));
    }
    auto set_addr = [&](unsigned long req, const std::uint8_t *addr4) {
        struct ifreq r {};
        std::strncpy(r.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
        auto *sin = reinterpret_cast<sockaddr_in *>(&r.ifr_addr);
        sin->sin_family = AF_INET;
        std::memcpy(&sin->sin_addr, addr4, 4);
        return ::ioctl(sock, req, &r);
    };
    if (set_addr(SIOCSIFADDR, ip.data()) < 0 || set_addr(SIOCSIFNETMASK, netmask.data()) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("Failed to configure tun interface address:" + std::string(std::strerror(err)));
    }
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("Failed to read interface flags:" + std::string(std::strerror(err)));
    }
    ifr.ifr_flags |= IFF_UP;
    if (::ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("Failed to enable interface:" + std::string(std::strerror(err)));
    }
    ::close(sock);

    read_thread_ = std::thread(&TunBackend::read_loop, this);
}

TunBackend::~TunBackend() {
    stop_ = true;
    // Join before close: close() does not interrupt another thread's blocking read (fd reference remains on the
    // kernel file object); directly closing makes join wait forever (process cannot exit). The poll
    // loop returns within one timeout period after stop_ is set, bounding join
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
    if (tun_fd_ >= 0) {
        ::close(tun_fd_); // Release fd, also destroying the tap interface
        tun_fd_ = -1;
    }
}

void TunBackend::send_frame(const std::uint8_t *data, std::size_t size) {
    count_rx(size);
    if (tun_fd_ < 0 || size > TUN_MAX_FRAME) {
        return; // Discard frames when closed or oversized (network data may be dropped, with TCP retransmission as a fallback)
    }
    // A tap device consumes one frame; write usually writes it entirely; treat partial writes/failure as dropped frames without retrying
    // (Linux write has warn_unused_result; casting to (void) cannot suppress GCC's warning,
    // so receive into a maybe_unused variable and explicitly leave it unread)
    [[maybe_unused]] const auto written = ::write(tun_fd_, data, size);
}

void TunBackend::read_loop() {
    std::uint8_t buf[TUN_MAX_FRAME];
    struct pollfd pfd {};
    pfd.fd = tun_fd_;
    pfd.events = POLLIN;
    while (!stop_) {
        // Timed poll: wait for data and periodically check stop_ (destructor join needs prompt return)
        int r = ::poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // Invalid fd (interface destroyed, etc.)
        }
        if (r == 0) {
            continue; // Timeout, recheck stop_
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }
        ssize_t n = ::read(tun_fd_, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        send_to_host(buf, static_cast<std::size_t>(n));
    }
}

} // namespace usbipdcpp
