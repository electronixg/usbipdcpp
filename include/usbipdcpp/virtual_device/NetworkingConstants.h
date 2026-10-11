#pragma once

#include <cstdint>

namespace usbipdcpp {

// ==================== Ethernet/TCP-IP protocol constants ====================

// length constant
constexpr std::size_t ETH_HLEN = 14; // Ethernet header length
constexpr std::size_t ETH_MIN_FRAME = 60; // Ethernet minimum frame length (14 header + 46 payload, excluding FCS)
constexpr std::size_t IPV4_HLEN = 20; // IPv4 Header length (no option)
constexpr std::size_t TCP_HLEN = 20; // TCP header length (no options)
constexpr std::size_t TCP_MAX_PAYLOAD = 1500; // response segment payload upper limit (not exceeding MTU)
// response frame buffer upper limit (14 header + 20 IP + 20 TCP + 1500 payload):
// Do not allocate the buffer based on ETH_MIN_FRAME: response payloads such as ICMP echo may approach the MTU;
// an array sized to the minimum frame length causes out-of-bounds memcpy (glibc _FORTIFY_SOURCE aborts directly)
constexpr std::size_t ETH_FRAME_MAX = ETH_HLEN + IPV4_HLEN + TCP_HLEN + TCP_MAX_PAYLOAD;

// Ethernet type (EtherType)
constexpr std::uint16_t ETH_TYPE_ARP = 0x0806;
constexpr std::uint16_t ETH_TYPE_IPV4 = 0x0800;

// IPv4 protocol number (RFC 790)
constexpr std::uint8_t IP_PROTO_ICMP = 1;
constexpr std::uint8_t IP_PROTO_TCP = 6;

// ARP opcode (RFC 826)
constexpr std::uint16_t ARP_OP_REQUEST = 1;
constexpr std::uint16_t ARP_OP_REPLY = 2;

// ICMP type (RFC 792)
constexpr std::uint8_t ICMP_ECHO_REQUEST = 8;
constexpr std::uint8_t ICMP_ECHO_REPLY = 0;

// TCP flag (RFC 793)
constexpr std::uint8_t TCP_FLAG_FIN = 0x01;
constexpr std::uint8_t TCP_FLAG_SYN = 0x02;
constexpr std::uint8_t TCP_FLAG_PSH = 0x08;
constexpr std::uint8_t TCP_FLAG_ACK = 0x10;

// ==================== Protocol header structure ====================
// Fields follow the wire format (multibyte fields in network byte order); access named members instead of calculating byte offsets manually;
// keep network-order fields unchanged in the struct and convert explicitly using bswap-like functions on reads.

#pragma pack(push, 1)
/// Ethernet header (14 bytes)
struct EthHeader {
    std::uint8_t dst[6];
    std::uint8_t src[6];
    std::uint16_t ethertype; // network byte order (0x0806 ARP / 0x0800 IPv4)
};
static_assert(sizeof(EthHeader) == ETH_HLEN, "Ethernet header fixed 14 Bytes");

/// ARP Header (28 Bytes, RFC 826, Hardware Type Ethernet + Protocol IPv4)
struct ArpHeader {
    std::uint16_t htype; // network byte order, 1 = Ethernet
    std::uint16_t ptype; // network byte order, 0x0800 = IPv4
    std::uint8_t hlen; // Hardware address length, 6
    std::uint8_t plen; // Protocol address length, 4
    std::uint16_t op; // network byte order, 1 = request, 2 = reply
    std::uint8_t sha[6];
    std::uint8_t spa[4];
    std::uint8_t tha[6];
    std::uint8_t tpa[4];
};
static_assert(sizeof(ArpHeader) == 28, "ARP header fixed 28 bytes");

/// IPv4 header (20 bytes, no options, RFC 791)
struct Ipv4Header {
    std::uint8_t version_ihl; // high 4 bit version=4, low 4 bit IHL (×4 byte)
    std::uint8_t tos;
    std::uint16_t total_len; // network byte order, entire IP packet length
    std::uint16_t id; // network byte order
    std::uint16_t frag_offset; // network byte order (including DF/MF flag)
    std::uint8_t ttl;
    std::uint8_t proto; // 1 ICMP / 6 TCP
    std::uint16_t checksum; // network byte order
    std::uint8_t src[4];
    std::uint8_t dst[4];
};
static_assert(sizeof(Ipv4Header) == IPV4_HLEN, "IPv4 header fixed 20 bytes");

/// TCP header (20 bytes, no options, RFC 793)
struct TcpHeader {
    std::uint16_t src_port; // network byte order
    std::uint16_t dst_port; // network byte order
    std::uint32_t seq; // network byte order
    std::uint32_t ack; // network byte order
    std::uint8_t offset_flags_hi; // high 4 bit data offset (×4 bytes), low 4 bit reserved
    std::uint8_t flags; // FIN/SYN/RST/PSH/ACK/URG bit
    std::uint16_t window; // network byte order
    std::uint16_t checksum; // network byte order
    std::uint16_t urgent; // network byte order
};
static_assert(sizeof(TcpHeader) == TCP_HLEN, "TCP header fixed 20 bytes");

/// ICMP echo header (8 bytes, RFC 792)
struct IcmpEchoHeader {
    std::uint8_t type; // 8 = echo request,0 = echo reply
    std::uint8_t code;
    std::uint16_t checksum; // network byte order
    std::uint16_t id; // network byte order
    std::uint16_t seq; // network byte order
};
static_assert(sizeof(IcmpEchoHeader) == 8, "ICMP echo header fixed 8 bytes");
#pragma pack(pop)

} // namespace usbipdcpp
