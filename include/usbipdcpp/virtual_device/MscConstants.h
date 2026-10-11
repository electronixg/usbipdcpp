#pragma once

#include <cstdint>

namespace usbipdcpp {

#pragma pack(push, 1)
struct CBW {
    std::uint32_t dCBWSignature; // 0x43425355 "USBC"
    std::uint32_t dCBWTag;
    std::uint32_t dCBWDataTransferLength;
    std::uint8_t bmCBWFlags; // bit 7: 0=OUT, 1=IN
    std::uint8_t bCBWLUN;
    std::uint8_t bCBWCBLength;
    std::uint8_t CBWCB[16];
};

struct CSW {
    std::uint32_t dCSWSignature; // 0x53425355 "USBS"
    std::uint32_t dCSWTag;
    std::uint32_t dCSWDataResidue;
    std::uint8_t bCSWStatus; // 0=passed, 1=failed, 2=phase error
};
#pragma pack(pop)

enum class BotState : std::uint8_t {
    Idle,
    DataIn,
    DataOut,
    Status,
};

/// Bulk-Only Transport Signature
inline constexpr std::uint32_t CBW_SIGNATURE = 0x43425355; // "USBC"
inline constexpr std::uint32_t CSW_SIGNATURE = 0x53425355; // "USBS"

/// SCSI command code
namespace ScsiCmd {
    inline constexpr std::uint8_t TestUnitReady = 0x00;
    inline constexpr std::uint8_t RequestSense = 0x03;
    inline constexpr std::uint8_t Inquiry = 0x12;
    inline constexpr std::uint8_t ModeSense6 = 0x1A;
    inline constexpr std::uint8_t StartStopUnit = 0x1B;
    inline constexpr std::uint8_t PreventAllowMediumRemoval = 0x1E;
    inline constexpr std::uint8_t ReadFormatCapacities = 0x23;
    inline constexpr std::uint8_t ReadCapacity10 = 0x25;
    inline constexpr std::uint8_t ReadCapacity16 = 0x9E;
    inline constexpr std::uint8_t Read10 = 0x28;
    inline constexpr std::uint8_t Write10 = 0x2A;
    inline constexpr std::uint8_t Verify10 = 0x2F;
    inline constexpr std::uint8_t SynchronizeCache = 0x35;
    inline constexpr std::uint8_t WriteSame10 = 0x41;
    inline constexpr std::uint8_t Unmap = 0x42;
    inline constexpr std::uint8_t ModeSense10 = 0x5A;
    inline constexpr std::uint8_t AtaPassThrough = 0x85;
    inline constexpr std::uint8_t WriteSame16 = 0x93;
} // namespace ScsiCmd

/// SCSI data are all in big-endian order, uniformly using byte array fields + the following auxiliary reading and writing,
/// Avoid little-endian machine byte order confusion caused by direct filling of structure fields with integers
inline void put_be16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put_be24(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v);
}

inline void put_be32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline void put_be64(std::uint8_t *p, std::uint64_t v) {
    put_be32(p, static_cast<std::uint32_t>(v >> 32));
    put_be32(p + 4, static_cast<std::uint32_t>(v));
}

inline std::uint16_t get_be16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t get_be32(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

inline std::uint64_t get_be64(const std::uint8_t *p) {
    return (std::uint64_t(get_be32(p)) << 32) | get_be32(p + 4);
}

#pragma pack(push, 1)

/// INQUIRY standard response data (SPC-4, 36 bytes)
struct InquiryData {
    std::uint8_t device_type;        // byte 0: 0x00 = direct access to block device
    std::uint8_t rmb;                // byte 1: bit7 = removable media
    std::uint8_t version;            // byte 2:0x07 = SPC-4
    std::uint8_t hisup_format;       // byte 3: bit7 = HiSup, bit3-0 = response data format
    std::uint8_t additional_length;  // byte 4: additional length = 31
    std::uint8_t reserved[2];        // byte 5-6
    std::uint8_t cmdque;             // byte 7: bit1 = command queue
    std::uint8_t vendor_id[8];       // byte 8-15: manufacturer identification
    std::uint8_t product_id[16];     // byte 16-31: product identification
    std::uint8_t product_revision[4]; // byte 32-35: product version
};

/// VPD 0x00 Supported VPD page list
struct VpdSupportedPages {
    std::uint8_t device_type; // 0x00
    std::uint8_t page_code;   // 0x00
    std::uint8_t page_length; // Number of supported pages
    std::uint8_t pages[5];    // 0x00 0x80 0xB0 0xB1 0xB2
};

/// VPD 0x80 unit serial number
struct VpdUnitSerialNumber {
    std::uint8_t device_type; // 0x00
    std::uint8_t page_code;   // 0x80
    std::uint8_t page_length; // serial number length
    std::uint8_t serial[32];  // Serial number (truncate excess, zero-pad if too short)
};

/// VPD 0xB0 block device characteristics (SBC-4): All zeros = non-rotating media, no special characteristics
struct VpdBlockDeviceCharacteristics {
    std::uint8_t device_type;   // 0x00
    std::uint8_t page_code;     // 0xB0
    std::uint8_t page_length[2]; // 0x00 0x3C
    std::uint8_t data[60];      // all 0
};

/// VPD 0xB1 block limit (SBC-4, 64 bytes)
struct VpdBlockLimits {
    std::uint8_t device_type;                  // 0x00
    std::uint8_t page_code;                    // 0xB1
    std::uint8_t page_length[2];               // 0x00 0x3C
    std::uint8_t medium_rotation_rate[2];      // byte 4-5: 0 = non-rotating media
    std::uint8_t reserved0[2];                 // byte 6-7
    std::uint8_t opt_transfer_granularity[4];  // byte 8-11
    std::uint8_t max_transfer_length[4];       // byte 12-15
    std::uint8_t opt_transfer_length[4];       // byte 16-19
    std::uint8_t max_prefetch_length[4];       // byte 20-23
    std::uint8_t max_unmap_lba_count[4];       // byte 24-27: Maximum number of LBAs in a single UNMAP
    std::uint8_t max_unmap_block_desc_count[4]; // byte 28-31: UNMAP maximum number of descriptors
    std::uint8_t opt_unmap_granularity[4];     // byte 32-35: UNMAP optimal granularity (LBA)
    std::uint8_t unmap_granularity_alignment[4]; // byte 36-39:bit31 = UGAVALID
    std::uint8_t max_write_same_length[4];     // byte 40-43: Maximum number of blocks in a single WRITE SAME
    std::uint8_t max_atomic_transfer_length[4]; // byte 44-47
    std::uint8_t atomic_alignment[4];          // byte 48-51
    std::uint8_t atomic_transfer_granularity[4]; // byte 52-55
    std::uint8_t max_atomic_boundary[4];       // byte 56-59
    std::uint8_t max_atomic_boundary_granularity[4]; // byte 60-63
};

/// VPD 0xB2 Logic Block Allocation (SBC-4)
struct VpdLogicalBlockProvisioning {
    std::uint8_t device_type;    // 0x00
    std::uint8_t page_code;      // 0xB2
    std::uint8_t page_length[2]; // 0x00 0x04
    std::uint8_t reserved;       // byte 4
    std::uint8_t lbpu;           // byte 5: bit7 = LBPU (supports UNMAP)
    std::uint8_t provisioning;   // byte 6: allocation related flags
    std::uint8_t reserved2;      // byte 7
};

/// REQUEST SENSE fixed format response (18 bytes)
struct SenseData {
    std::uint8_t valid_response_code;      // byte 0: 0x70 = fixed format, valid
    std::uint8_t reserved0;                // byte 1
    std::uint8_t sense_key;                // byte 2
    std::uint8_t info[4];                  // byte 3-6
    std::uint8_t additional_length;        // byte 7:10
    std::uint8_t command_specific[4];      // byte 8-11
    std::uint8_t asc;                      // byte 12
    std::uint8_t ascq;                     // byte 13
    std::uint8_t fru;                      // byte 14
    std::uint8_t sense_key_specific[3];    // byte 15-17
};

/// MODE SENSE (6) response (4 byte mode header, no block descriptor and mode page)
struct ModeSense6Data {
    std::uint8_t mode_data_length;       // byte 0: remaining length = len-1
    std::uint8_t medium_type;            // byte 1:0
    std::uint8_t wp;                     // byte 2: bit7 = write protected
    std::uint8_t block_descriptor_length; // byte 3:0
};

/// MODE SENSE (10) response (8 byte mode header)
struct ModeSense10Data {
    std::uint8_t mode_data_length[2];    // byte 0-1: remaining length = len-2 (big-endian)
    std::uint8_t medium_type;            // byte 2:0
    std::uint8_t wp;                     // byte 3: bit7 = write protected
    std::uint8_t block_descriptor_length[4]; // byte 4-7:0
};

/// READ CAPACITY (10) response
struct ReadCapacity10Data {
    std::uint8_t last_lba[4];    // Last LBA (big-endian)
    std::uint8_t block_size[4];  // block size (big-endian)
};

/// READ CAPACITY (16) response
struct ReadCapacity16Data {
    std::uint8_t last_lba[8];    // Last LBA (big-endian)
    std::uint8_t block_size[4];  // block size (big-endian)
    std::uint8_t prot_info;      // byte 12
    std::uint8_t p_type;         // byte 13
    std::uint8_t reserved[2];    // byte 14-15
};

/// READ FORMAT CAPACITIES response (current capacity + a format descriptor)
struct ReadFormatCapacitiesData {
    std::uint8_t reserved[2];     // byte 0-1
    std::uint8_t list_length[2];  // byte 2-3: total length of capacity list (big-endian)
    std::uint8_t capacity[4];     // byte 4-7: number of blocks (big-endian)
    std::uint8_t format_type;     // byte 8: 0x02 = Formatted media
    std::uint8_t block_size[3];   // byte 9-11: block size (big-endian 24 bits)
};

/// block descriptor in UNMAP parameter (16 bytes, big-endian)
struct UnmapBlockDescriptor {
    std::uint8_t lba[8];          // starting LBA (big-endian)
    std::uint8_t block_count[4];  // Block count (big-endian)
    std::uint8_t reserved[4];     // 0
};

/// READ/WRITE (10) CDB(0x28/0x2A)
struct ReadWrite10Cdb {
    std::uint8_t opcode;       // 0x28 / 0x2A
    std::uint8_t flags;        // byte 1
    std::uint8_t lba[4];       // byte 2-5 (big-endian)
    std::uint8_t group;        // byte 6
    std::uint8_t block_count[2]; // byte 7-8 (big-endian, 0 = 256)
    std::uint8_t control;      // byte 9
};

/// WRITE SAME (10) CDB(0x41)
struct WriteSame10Cdb {
    std::uint8_t opcode;       // 0x41
    std::uint8_t flags;        // byte 1:bit0 = UNMAP
    std::uint8_t lba[4];       // byte 2-5 (big-endian)
    std::uint8_t reserved;     // byte 6
    std::uint8_t block_count[2]; // byte 7-8 (big-endian, 0 = to the end of the media)
    std::uint8_t control;      // byte 9
};

/// WRITE SAME (16) CDB(0x93)
struct WriteSame16Cdb {
    std::uint8_t opcode;       // 0x93
    std::uint8_t flags;        // byte 1:bit0 = UNMAP
    std::uint8_t lba[8];       // byte 2-9 (big-endian)
    std::uint8_t block_count[4]; // byte 10-13 (big-endian, 0 = to the end of the media)
    std::uint8_t group;        // byte 14
    std::uint8_t control;      // byte 15
};

#pragma pack(pop)

} // namespace usbipdcpp
