#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/NetworkingConstants.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

/// Minimal Ethernet backend: ARP replies + ICMP echo + TCP echo (demonstrates "a host accessing device-side services
/// through a virtual network adapter")
/// Pure user-space Ethernet frame processing (no platform network API dependencies): after the host attaches the adapter and configures
/// an IP in the same subnet, it can ping the device-side IP and use `nc <device-IP> <tcp_port>` to connect to the TCP echo service
class USBIPDCPP_API EthernetEchoBackend : public NetworkBackend {
public:
    /**
     * @param mac Device-side MAC address (source address of response frames)
     * @param ip Device-side IPv4 address (dotted decimal)
     * @param tcp_port TCP echo service port
     */
    EthernetEchoBackend(std::array<std::uint8_t, 6> mac, std::array<std::uint8_t, 4> ip, std::uint16_t tcp_port);

    void send_frame(const std::uint8_t *data, std::size_t size) override;

private:
    /// Process one frame (called from send_frame, entirely synchronous)
    void handle_frame(const std::uint8_t *data, std::size_t size);

    /// ARP request (Ethernet 0x0806) → return ARP reply
    void handle_arp(const std::uint8_t *frame, std::size_t size);

    /// IPv4 (Ethernet 0x0800) → dispatch ICMP/TCP
    void handle_ipv4(const std::uint8_t *frame, std::size_t size);

    /// ICMP echo request → echo reply
    void handle_icmp(const Ipv4Header *ip, const std::uint8_t *icmp, std::size_t icmp_len);

    /// TCP echo state machine (SYN → SYN-ACK, data echo, FIN close)
    void handle_tcp(const Ipv4Header *ip, const std::uint8_t *tcp, std::size_t tcp_len);

    /// Send a frame (Ethernet header + payload, padded to minimum frame length of 60 bytes)
    void send_frame_to_host(const std::uint8_t *dst_mac, std::uint16_t ethertype, const std::uint8_t *payload,
                            std::size_t payload_len);

    /// Send an IPv4 packet (assemble IP header + checksum)
    void send_ipv4(std::uint8_t proto, const std::uint8_t *dst_ip, const std::uint8_t *payload,
                   std::size_t payload_len);

    /// 16-bit one's-complement sum checksum (IP/TCP checksum)
    static std::uint16_t checksum(const std::uint8_t *data, std::size_t size, std::uint32_t initial = 0);

    std::array<std::uint8_t, 6> mac_;
    std::array<std::uint8_t, 4> ip_;
    std::uint16_t tcp_port_;

    // TCP session state (currently only one connection supported, simplifying the demonstration)
    bool tcp_established_ = false;
    std::uint8_t peer_mac_[6]{}; // Peer MAC (recorded for each frame by handle_frame, one peer on the local link)
    std::uint32_t tcp_my_seq_ = 0x10000000; // Our sequence number (fixed initial value suffices for demonstration)
    std::uint32_t tcp_peer_seq_ = 0; // Peer's next expected sequence number (next byte after data sent by the peer)
};

} // namespace usbipdcpp
