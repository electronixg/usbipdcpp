#pragma once

#include <cstddef>
#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {
// RNDIS (Remote NDIS) protocol constants and message structures.
// refers to Microsoft RNDIS 2.0 specification and kernel rndis.c/include/linux/rndis.h;
// device descriptor uses ACM shell (kernel f_rndis.c: control interface matches ACM instead of Ethernet),
// does not require Microsoft's RNDIS proprietary descriptor (0x24/0x20)

// RNDIS message type (highest bit = completion flag)
enum class RndisMessageType : std::uint32_t {
    Packet = 0x00000001, // Data channel encapsulation
    Init = 0x00000002,
    InitComplete = 0x80000002,
    Halt = 0x00000003,
    Query = 0x00000004,
    QueryComplete = 0x80000004,
    Set = 0x00000005,
    SetComplete = 0x80000005,
    Reset = 0x00000006,
    ResetComplete = 0x80000006,
    Indicate = 0x00000007, // device active status notification (MEDIA_CONNECT, etc.)
    Keepalive = 0x00000008,
    KeepaliveComplete = 0x80000008,
};

// RNDIS status code (the host judges the command result based on this)
enum class RndisStatus : std::uint32_t {
    Success = 0x00000000,
    MediaConnect = 0x4001000B,
    MediaDisconnect = 0x4001000C,
    NotSupported = 0xC00000BB,
    InvalidData = 0xC0010015,
};

// Enumeration → wire-format value (for aggregate initialization of packed struct fields; enum class cannot implicitly convert to integer)
inline constexpr std::uint32_t rndis_msg(RndisMessageType t) {
    return static_cast<std::uint32_t>(t);
}
inline constexpr std::uint32_t rndis_status(RndisStatus s) {
    return static_cast<std::uint32_t>(s);
}

// ==================== OID constant ====================
// value is taken from kernel include/linux/rndis.h; name maintains NDIS convention (OID_GEN_* / OID_802_3_*)
inline constexpr std::uint32_t OID_GEN_SUPPORTED_LIST = 0x00010101;
inline constexpr std::uint32_t OID_GEN_HARDWARE_STATUS = 0x00010102;
inline constexpr std::uint32_t OID_GEN_MEDIA_SUPPORTED = 0x00010103;
inline constexpr std::uint32_t OID_GEN_MEDIA_IN_USE = 0x00010104;
inline constexpr std::uint32_t OID_GEN_MAXIMUM_FRAME_SIZE = 0x00010106;
inline constexpr std::uint32_t OID_GEN_LINK_SPEED = 0x00010107;
inline constexpr std::uint32_t OID_GEN_TRANSMIT_BLOCK_SIZE = 0x0001010A;
inline constexpr std::uint32_t OID_GEN_RECEIVE_BLOCK_SIZE = 0x0001010B;
inline constexpr std::uint32_t OID_GEN_VENDOR_ID = 0x0001010C;
inline constexpr std::uint32_t OID_GEN_VENDOR_DESCRIPTION = 0x0001010D;
inline constexpr std::uint32_t OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E;
inline constexpr std::uint32_t OID_GEN_MAXIMUM_TOTAL_SIZE = 0x00010111;
inline constexpr std::uint32_t OID_GEN_MAC_OPTIONS = 0x00010113;
inline constexpr std::uint32_t OID_GEN_MEDIA_CONNECT_STATUS = 0x00010114;
inline constexpr std::uint32_t OID_GEN_VENDOR_DRIVER_VERSION = 0x00010116;
inline constexpr std::uint32_t OID_GEN_PHYSICAL_MEDIUM = 0x00010202;
inline constexpr std::uint32_t OID_GEN_XMIT_OK = 0x00020101;
inline constexpr std::uint32_t OID_GEN_RCV_OK = 0x00020102;
inline constexpr std::uint32_t OID_GEN_XMIT_ERROR = 0x00020103;
inline constexpr std::uint32_t OID_GEN_RCV_ERROR = 0x00020104;
inline constexpr std::uint32_t OID_GEN_RCV_NO_BUFFER = 0x00020105;
inline constexpr std::uint32_t OID_802_3_PERMANENT_ADDRESS = 0x01010101;
inline constexpr std::uint32_t OID_802_3_CURRENT_ADDRESS = 0x01010102;
inline constexpr std::uint32_t OID_802_3_MULTICAST_LIST = 0x01010103;
inline constexpr std::uint32_t OID_802_3_MAXIMUM_LIST_SIZE = 0x01010104;
inline constexpr std::uint32_t OID_802_3_MAC_OPTIONS = 0x01010105;
inline constexpr std::uint32_t OID_802_3_RCV_ERROR_ALIGNMENT = 0x01020101;
inline constexpr std::uint32_t OID_802_3_XMIT_ONE_COLLISION = 0x01020102;
inline constexpr std::uint32_t OID_802_3_XMIT_MORE_COLLISIONS = 0x01020103;

// ==================== Protocol constants ====================
// version and device capabilities (aligned kernel rndis.h: connectionless network card + 802.3 media)
inline constexpr std::uint32_t RNDIS_MAJOR_VERSION = 0x00000001;
inline constexpr std::uint32_t RNDIS_MINOR_VERSION = 0x00000000;
inline constexpr std::uint32_t RNDIS_DF_CONNECTIONLESS = 0x00000001;
inline constexpr std::uint32_t RNDIS_MEDIUM_802_3 = 0x00000000;
// Maximum frame length (including Ethernet header, excluding FCS) and upper limit of single message (aligned core rndis.h)
inline constexpr std::uint32_t RNDIS_MAXIMUM_FRAME_SIZE = 1518;
inline constexpr std::uint32_t RNDIS_MAX_TOTAL_SIZE = 1558;
// MaxTransferSize of  INIT_C: kernel rndis.c back mtu+ethhdr+rndis_header+22 = 1580.
// Host side requirement ≥ hard_mtu (1524), if lower than 1518 the binding will fail
inline constexpr std::uint32_t RNDIS_MAX_TRANSFER_SIZE = 1580;
// The upper limit of the response queue: the number of normal handshake queues is single digit, the upper limit prevents malicious hosts from unlimited memory usage
// (Align channel class "discard the oldest" anti-stacking convention)
inline constexpr std::size_t RNDIS_RESPONSE_QUEUE_LIMIT = 64;
// host default packet filter (rndis_host.c RNDIS_DEFAULT_FILTER):
// DIRECTED|BROADCAST|ALL_MULTICAST|PROMISCUOUS
inline constexpr std::uint16_t RNDIS_DEFAULT_FILTER = 0x2D;

/// Host direction RNDIS_MSG_PACKET header length (24 bytes, aligned to RNDIS_HEADER of rndis_host.h):
/// The host only sends the first 6 fields, and the remaining fields of the device direction header (RndisPacketHeader 44 bytes) are not sent.
inline constexpr std::size_t RNDIS_HOST_HEADER_LEN = 24;
// MAC option (OID_GEN_MAC_OPTIONS): RECEIVE_SERIALIZED|FULL_DUPLEX (aligned core)
inline constexpr std::uint32_t RNDIS_MAC_OPTIONS_SERIALIZED_FULL_DUPLEX = 0x12;

// ==================== Message structure ====================
// The wire format is always little-endian. Outgoing messages use append_to serialization (htole automatically converts to little-endian);
// Inbound message memcpy to the structure and then the field value is directly accessed (assuming little-endian host, same as MSC's CBW)

#pragma pack(push, 1)
/// 12 byte prefix common to all messages
struct RndisMessageHeader {
    std::uint32_t message_type;
    std::uint32_t message_length; // Number of bytes of the entire message (including header)
    std::uint32_t request_id; // command echo number (not used for HALT/RESET/INDICATE)
};
static_assert(sizeof(RndisMessageHeader) == 12, "RNDIS message header fixed 12 bytes");

/// RNDIS_MSG_INIT (host → device, 24 bytes)
struct RndisInitMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t major_version;
    std::uint32_t minor_version;
    std::uint32_t max_transfer_size; // Host-side rx buffer; the device may ignore it
};
static_assert(sizeof(RndisInitMsg) == 24, "RNDIS_MSG_INIT Fixed 24 Bytes");

/// RNDIS_MSG_INIT_C (device → host, 52 bytes)
struct RndisInitCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;
    std::uint32_t major_version;
    std::uint32_t minor_version;
    std::uint32_t device_flags; // RNDIS_DF_CONNECTIONLESS
    std::uint32_t medium; // RNDIS_MEDIUM_802_3
    std::uint32_t max_packets_per_transfer; // 1: transfer one packet at a time
    std::uint32_t max_transfer_size;
    std::uint32_t packet_alignment_factor; // 0: No alignment required
    std::uint32_t af_list_offset; // 0: No address family list
    std::uint32_t af_list_size;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status, major_version, minor_version,
                            device_flags, medium, max_packets_per_transfer, max_transfer_size,
                            packet_alignment_factor, af_list_offset, af_list_size);
    }
};
static_assert(sizeof(RndisInitCmplt) == 52, "RNDIS_MSG_INIT_C Fixed 52 Bytes");

/// RNDIS_MSG_QUERY (host → device, 28 bytes + input buffer)
/// Input buffer offset relative to RequestID field (message byte 8), data starting point during parsing = 8 + offset
struct RndisQueryMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t oid;
    std::uint32_t information_buffer_length; // input buffer length (ActiveSync comes with 48 byte quirk value)
    std::uint32_t information_buffer_offset; // input buffer offset (relative to byte 8)
    std::uint32_t device_vc_handle; // 0
};
static_assert(sizeof(RndisQueryMsg) == 28, "RNDIS_MSG_QUERY Fixed 28 Bytes");

/// RNDIS_MSG_QUERY_C (device → host, 24 bytes + data; InformationBufferOffset fixed 16)
struct RndisQueryCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;
    std::uint32_t information_buffer_length;
    std::uint32_t information_buffer_offset; // 16: Data follows 24 byte header

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status, information_buffer_length,
                            information_buffer_offset);
    }
};
static_assert(sizeof(RndisQueryCmplt) == 24, "RNDIS_MSG_QUERY_C Header fixed 24 Byte");

/// RNDIS_MSG_SET (host → device, 28 bytes + input buffer; offset semantics are the same as QUERY)
struct RndisSetMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t oid;
    std::uint32_t information_buffer_length;
    std::uint32_t information_buffer_offset;
    std::uint32_t device_vc_handle;
};
static_assert(sizeof(RndisSetMsg) == 28, "RNDIS_MSG_SET Fixed 28 Bytes");

/// RNDIS_MSG_SET_C (device → host, 16 bytes)
struct RndisSetCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status);
    }
};
static_assert(sizeof(RndisSetCmplt) == 16, "RNDIS_MSG_SET_C Fixed 16 Bytes");

/// RNDIS_MSG_RESET_C (device → host, 16 bytes; no RequestID)
struct RndisResetCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t status;
    std::uint32_t addressing_reset; // 1 = Address/status information has been reset

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, status, addressing_reset);
    }
};
static_assert(sizeof(RndisResetCmplt) == 16, "RNDIS_MSG_RESET_C Fixed 16 Bytes");

/// RNDIS_MSG_KEEPALIVE_C (device → host, 16 bytes)
struct RndisKeepaliveCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status);
    }
};
static_assert(sizeof(RndisKeepaliveCmplt) == 16, "RNDIS_MSG_KEEPALIVE_C Fixed 16 Bytes");

/// RNDIS_MSG_INDICATE (Device→Host Active Status Notification, 20 bytes; no RequestID)
struct RndisIndicateStatusMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t status; // MEDIA_CONNECT / MEDIA_DISCONNECT
    std::uint32_t status_buffer_length; // 0
    std::uint32_t status_buffer_offset; // 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, status, status_buffer_length, status_buffer_offset);
    }
};
static_assert(sizeof(RndisIndicateStatusMsg) == 20, "RNDIS_MSG_INDICATE fixed 20 bytes");

/// RNDIS_MSG_PACKET (data channel encapsulation header, 44 bytes, device → host direction)
/// Data start = data_offset field position + data_offset value (consistent in both directions):
/// The device → the host sends 36 → the data starts from the byte 44; the host → the device sends 16 → the data starts from the byte 24
/// (See data_start_offset for data starting algorithm, alignment kernel rndis_add_hdr/rm_hdr).
/// Kernel implementations set all OOB/PerPacket fields to 0 (no TCP checksum offload); the device only needs to tolerate nonzero values.
/// Only the first 24 bytes are sent in the host direction (RNDIS_HEADER aligned with rndis_host.h, the remaining fields are not sent):
/// Read-only MessageLength/DataOffset/DataLength (first 16 bytes) by pointer when parsing host messages,
/// Do not memcpy the entire structure - insufficient small frame messages 44 bytes will be read out of bounds (ASan actual measurement)
struct RndisPacketHeader {
    std::uint32_t message_type;
    std::uint32_t message_length; // 44 + data_length (excluding tail padding)
    std::uint32_t data_offset;
    std::uint32_t data_length;
    std::uint32_t oob_data_offset;
    std::uint32_t oob_data_length;
    std::uint32_t num_oob_data_elements;
    std::uint32_t per_packet_info_offset;
    std::uint32_t per_packet_info_length;
    std::uint32_t vc_handle;
    std::uint32_t reserved;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, data_offset, data_length, oob_data_offset,
                            oob_data_length, num_oob_data_elements, per_packet_info_offset,
                            per_packet_info_length, vc_handle, reserved);
    }
};
static_assert(sizeof(RndisPacketHeader) == 44, "RNDIS_MSG_PACKET Header fixed 44 Byte");

/// Data starting offset in message = data_offset field position + data_offset value. RNDIS protocol
/// defines DataOffset's base at byte 8 of the message header (after MessageType+MessageLength):
/// device direction 36 → data starting from 44, host direction 16 → data starting from 24 (aligned core
/// rndis_add_hdr writes RNDIS_HEADER-8, and rndis_rm_hdr strips DataOffset+8)
[[nodiscard]] inline std::size_t data_start_offset(const RndisPacketHeader &h) {
    return offsetof(RndisPacketHeader, data_offset) + h.data_offset;
}
#pragma pack(pop)

/// RESPONSE_AVAILABLE notification: Interrupt IN endpoint sends 8 bytes (two LE32: {1, 0}),
/// The host then uses GET_ENCAPSULATED_RESPONSE to retrieve queued responses (consistent with the kernel resp_avail callback)
inline data_type make_rndis_response_available_notification() {
    return {1, 0, 0, 0, 0, 0, 0, 0};
}
} // namespace usbipdcpp
