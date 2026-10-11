#include "usbipdcpp/virtual_device/network_backends/EthernetEchoBackend.h"

#include <cstring>

namespace usbipdcpp {

namespace {

// Byte-order conversion: protocol multibyte fields are stored in network order (big-endian); convert explicitly on access
std::uint16_t bswap16(std::uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

std::uint32_t bswap32(std::uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

/// Read a big-endian 16-bit value from a byte stream (memcpy ensures unaligned safety, for checksum)
std::uint16_t read_be16(const std::uint8_t *p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap16(v);
}

} // namespace

std::uint16_t EthernetEchoBackend::checksum(const std::uint8_t *data, std::size_t size, std::uint32_t initial) {
    // 16-bit one's-complement sum checksum (shared by IP/TCP/ICMP)
    // initial is a precalculated accumulated sum for pseudoheaders, etc. (not inverted); continue accumulating, then fold and invert
    std::uint32_t sum = initial;
    while (size > 1) {
        sum += read_be16(data);
        data += 2;
        size -= 2;
    }
    if (size == 1) {
        sum += static_cast<std::uint32_t>(data[0]) << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}

EthernetEchoBackend::EthernetEchoBackend(std::array<std::uint8_t, 6> mac, std::array<std::uint8_t, 4> ip,
                                         std::uint16_t tcp_port) :
    mac_(mac), ip_(ip), tcp_port_(tcp_port) {
}

void EthernetEchoBackend::send_frame(const std::uint8_t *data, std::size_t size) {
    count_rx(size);
    handle_frame(data, size);
}

void EthernetEchoBackend::handle_frame(const std::uint8_t *data, std::size_t size) {
    if (size < ETH_HLEN) {
        return; // Bad frame: shorter than an Ethernet header
    }
    const auto *eth = reinterpret_cast<const EthHeader *>(data);
    // One peer on the local link: record frame source MAC, send subsequent replies (ICMP/TCP) to it
    std::memcpy(peer_mac_, eth->src, 6);
    switch (bswap16(eth->ethertype)) {
        case ETH_TYPE_ARP:
            handle_arp(data, size);
            break;
        case ETH_TYPE_IPV4:
            handle_ipv4(data, size);
            break;
        default:
            break; // Ignore other protocol types
    }
}

void EthernetEchoBackend::handle_arp(const std::uint8_t *frame, std::size_t size) {
    if (size < ETH_HLEN + sizeof(ArpHeader)) {
        return; // Shorter than an ARP header
    }
    const auto *arp = reinterpret_cast<const ArpHeader *>(frame + ETH_HLEN);
    // Handle only Ethernet/IPv4 ARP requests whose target IP is ours
    if (bswap16(arp->htype) != 1 || bswap16(arp->ptype) != ETH_TYPE_IPV4 || arp->hlen != 6 || arp->plen != 4) {
        return;
    }
    if (bswap16(arp->op) != ARP_OP_REQUEST || std::memcmp(arp->tpa, ip_.data(), 4) != 0) {
        return;
    }
    // Construct ARP reply: target = requester (swap hardware/protocol addresses)
    ArpHeader reply{};
    reply.htype = arp->htype; // Requester's values are already network-order; copy unchanged
    reply.ptype = arp->ptype;
    reply.hlen = 6;
    reply.plen = 4;
    reply.op = bswap16(ARP_OP_REPLY);
    std::memcpy(reply.sha, mac_.data(), 6); // sha = us
    std::memcpy(reply.spa, ip_.data(), 4); // spa = us
    std::memcpy(reply.tha, arp->sha, 6); // tha = requester
    std::memcpy(reply.tpa, arp->spa, 4); // tpa = requester
    send_frame_to_host(arp->sha, ETH_TYPE_ARP, reinterpret_cast<const std::uint8_t *>(&reply), sizeof(reply));
}

void EthernetEchoBackend::handle_ipv4(const std::uint8_t *frame, std::size_t size) {
    if (size < ETH_HLEN + sizeof(Ipv4Header)) {
        return; // Shorter than an IPv4 header
    }
    const auto *ip = reinterpret_cast<const Ipv4Header *>(frame + ETH_HLEN);
    std::size_t ihl = static_cast<std::size_t>(ip->version_ihl & 0x0F) * 4;
    std::size_t total_len = bswap16(ip->total_len);
    if (ihl < IPV4_HLEN || total_len < ihl || size < ETH_HLEN + total_len) {
        return; // Invalid header/total length or truncated frame
    }
    // Handle only packets addressed to us (no forwarding)
    if (std::memcmp(ip->dst, ip_.data(), 4) != 0) {
        return;
    }
    const std::uint8_t *payload = frame + ETH_HLEN + ihl;
    std::size_t payload_len = total_len - ihl;
    switch (ip->proto) {
        case IP_PROTO_ICMP:
            handle_icmp(ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            handle_tcp(ip, payload, payload_len);
            break;
        default:
            break;
    }
}

void EthernetEchoBackend::handle_icmp(const Ipv4Header *ip, const std::uint8_t *icmp, std::size_t icmp_len) {
    if (icmp_len < sizeof(IcmpEchoHeader) || icmp_len > TCP_MAX_PAYLOAD) {
        return; // Shorter than an ICMP echo header or segment exceeds the limit
    }
    const auto *echo = reinterpret_cast<const IcmpEchoHeader *>(icmp);
    if (echo->type != ICMP_ECHO_REQUEST || echo->code != 0) {
        return;
    }
    // Copy the whole segment (header+payload), change type, recalculate checksum: payload must be echoed unchanged;
    // copying only the header struct reads out of bounds (checksum and send both use icmp_len)
    std::uint8_t reply[TCP_MAX_PAYLOAD];
    std::memcpy(reply, icmp, icmp_len);
    auto *hdr = reinterpret_cast<IcmpEchoHeader *>(reply);
    hdr->type = ICMP_ECHO_REPLY;
    hdr->checksum = 0;
    hdr->checksum = bswap16(checksum(reply, icmp_len));
    send_ipv4(IP_PROTO_ICMP, ip->src, reply, icmp_len);
}

void EthernetEchoBackend::handle_tcp(const Ipv4Header *ip, const std::uint8_t *tcp, std::size_t tcp_len) {
    if (tcp_len < sizeof(TcpHeader)) {
        return; // Shorter than a TCP header
    }
    const auto *tcp_hdr = reinterpret_cast<const TcpHeader *>(tcp);
    if (bswap16(tcp_hdr->dst_port) != tcp_port_) {
        return; // Not our service port
    }
    std::size_t data_offset = static_cast<std::size_t>(tcp_hdr->offset_flags_hi >> 4) * 4;
    if (data_offset < sizeof(TcpHeader) || data_offset > tcp_len) {
        return;
    }
    std::uint32_t seq = bswap32(tcp_hdr->seq);
    std::uint32_t ack = bswap32(tcp_hdr->ack);
    std::uint8_t flags = tcp_hdr->flags;
    const std::uint8_t *payload = tcp + data_offset;
    std::size_t payload_len = tcp_len - data_offset;

    auto send_segment = [&](std::uint8_t seg_flags, std::uint32_t seg_seq, std::uint32_t seg_ack,
                            const std::uint8_t *seg_payload, std::size_t seg_payload_len) {
        // TCP header + payload (no options in response segment, data offset always 5)
        std::uint8_t segment[TCP_HLEN + TCP_MAX_PAYLOAD];
        auto *hdr = reinterpret_cast<TcpHeader *>(segment);
        hdr->src_port = bswap16(tcp_port_); // sport = us
        hdr->dst_port = tcp_hdr->src_port; // dport = peer's port (copy network-order unchanged)
        hdr->seq = bswap32(seg_seq);
        hdr->ack = bswap32(seg_ack);
        hdr->offset_flags_hi = static_cast<std::uint8_t>(5 << 4); // data offset = 5
        hdr->flags = seg_flags;
        hdr->window = bswap16(4096);
        hdr->checksum = 0;
        hdr->urgent = 0;
        if (seg_payload_len > 0) {
            std::memcpy(segment + TCP_HLEN, seg_payload, seg_payload_len);
        }
        // TCP checksum = pseudoheader (src/dst IP + 0 + proto + segment length) + segment
        std::uint32_t pseudo = 0;
        pseudo += static_cast<std::uint16_t>((ip->src[0] << 8) | ip->src[1]);
        pseudo += static_cast<std::uint16_t>((ip->src[2] << 8) | ip->src[3]);
        pseudo += static_cast<std::uint16_t>((ip->dst[0] << 8) | ip->dst[1]);
        pseudo += static_cast<std::uint16_t>((ip->dst[2] << 8) | ip->dst[3]);
        pseudo += IP_PROTO_TCP;
        pseudo += TCP_HLEN + seg_payload_len;
        hdr->checksum = bswap16(checksum(segment, TCP_HLEN + seg_payload_len, pseudo));
        send_ipv4(IP_PROTO_TCP, ip->src, segment, TCP_HLEN + seg_payload_len);
    };

    if ((flags & TCP_FLAG_SYN) && !tcp_established_) {
        // Three-way handshake step 1: return SYN-ACK (peer address already recorded by handle_frame)
        tcp_peer_seq_ = seq + 1;
        tcp_established_ = true;
        send_segment(TCP_FLAG_SYN | TCP_FLAG_ACK, tcp_my_seq_, tcp_peer_seq_, nullptr, 0);
    }
    else if (tcp_established_) {
        // Handle data echo first, then FIN
        if (payload_len > 0) {
            tcp_peer_seq_ = seq + payload_len;
            send_segment(TCP_FLAG_PSH | TCP_FLAG_ACK, tcp_my_seq_, tcp_peer_seq_, payload, payload_len);
            tcp_my_seq_ += payload_len;
        }
        if (flags & TCP_FLAG_FIN) {
            tcp_peer_seq_ = seq + payload_len + 1;
            send_segment(TCP_FLAG_ACK | TCP_FLAG_FIN, tcp_my_seq_, tcp_peer_seq_, nullptr, 0);
            tcp_established_ = false;
        }
        else if (payload_len == 0 && ack > tcp_my_seq_) {
            // Pure acknowledgment (we sent data): synchronize our sequence number
            tcp_my_seq_ = ack;
        }
    }
}

void EthernetEchoBackend::send_frame_to_host(const std::uint8_t *dst_mac, std::uint16_t ethertype,
                                             const std::uint8_t *payload, std::size_t payload_len) {
    // Size buffer for the maximum response frame (ICMP echo payload may approach MTU; minimum-frame sizing would overflow);
    // padding to minimum frame length 60 bytes (64 on the wire including FCS) only satisfies the Ethernet specification
    std::uint8_t frame[ETH_FRAME_MAX];
    auto *eth = reinterpret_cast<EthHeader *>(frame);
    std::memcpy(eth->dst, dst_mac, 6);
    std::memcpy(eth->src, mac_.data(), 6);
    eth->ethertype = bswap16(ethertype);
    if (payload_len > 0) {
        std::memcpy(frame + ETH_HLEN, payload, payload_len);
    }
    std::size_t total = ETH_HLEN + payload_len;
    if (total < ETH_MIN_FRAME) {
        std::memset(frame + total, 0, ETH_MIN_FRAME - total);
        total = ETH_MIN_FRAME;
    }
    // Sent frame count is accumulated in base class send_to_host
    send_to_host(frame, total);
}

void EthernetEchoBackend::send_ipv4(std::uint8_t proto, const std::uint8_t *dst_ip, const std::uint8_t *payload,
                                    std::size_t payload_len) {
    // 20-byte IPv4 header + payload (no fragmentation implemented; payload is far below MTU, so it is not triggered)
    std::uint8_t ip[IPV4_HLEN + TCP_MAX_PAYLOAD];
    auto *hdr = reinterpret_cast<Ipv4Header *>(ip);
    hdr->version_ihl = 0x45; // version 4, IHL 5
    hdr->tos = 0;
    hdr->total_len = bswap16(IPV4_HLEN + payload_len);
    hdr->id = 0; // No need to associate fragments
    hdr->frag_offset = bswap16(0x4000); // DF set (do not fragment)
    hdr->ttl = 64;
    hdr->proto = proto;
    hdr->checksum = 0;
    std::memcpy(hdr->src, ip_.data(), 4);
    std::memcpy(hdr->dst, dst_ip, 4);
    hdr->checksum = bswap16(checksum(ip, IPV4_HLEN));
    if (payload_len > 0) {
        std::memcpy(ip + IPV4_HLEN, payload, payload_len);
    }
    send_frame_to_host(peer_mac_, ETH_TYPE_IPV4, ip, IPV4_HLEN + payload_len);
}

} // namespace usbipdcpp
