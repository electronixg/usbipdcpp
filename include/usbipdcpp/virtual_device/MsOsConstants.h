#pragma once

#include <array>
#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {
// Microsoft OS 1.0 descriptor: String index 0xEE signature string ("MSFT100") + Compatible ID
// feature descriptor. Align kernel gadget's configfs os_desc support (composite.c's
// usb_os_string / fill_ext_compat); Windows relies on it for some device classes (such as RNDIS
// network card) loads the built-in driver

/// MS OS 1.0 String index of signature string (index of GET_DESCRIPTOR String)
inline constexpr std::uint8_t MS_OS_STRING_INDEX = 0xEE;
/// Compatible ID feature descriptor's wIndex (Vendor request)
inline constexpr std::uint16_t MS_OS_COMPAT_ID_WINDEX = 0x0004;
/// bMS_VendorCode default (kernel configfs os_desc convention, see os_desc/b_vendor_code)
inline constexpr std::uint8_t MS_OS_DEFAULT_VENDOR_CODE = 0x01;
/// UTF-16LE encoding of "MSFT100" (14 bytes, fixed by the Microsoft OS descriptor specification)
inline constexpr std::array<std::uint8_t, 14> MS_OS_SIGNATURE_UTF16LE = {
        0x4D, 0x00, 0x53, 0x00, 0x46, 0x00, 0x54, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00,
};

// ==================== Fixed length descriptor structure ====================
// The length of  is rigidly specified by the Microsoft OS descriptor specification, represented by a packed structure, and verified by static_assert.

#pragma pack(push, 1)
/// MS OS 1.0 signature string (string index 0xEE, fixed 18 bytes)
struct MsOsStringDesc {
    std::uint8_t b_length; // 18
    std::uint8_t b_descriptor_type; // 0x03 STRING
    std::array<std::uint8_t, 14> qw_signature; // "MSFT100" UTF-16LE
    std::uint8_t b_ms_vendor_code; // Compatible ID request number requested
    std::uint8_t b_pad; // 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, b_length, b_descriptor_type, qw_signature, b_ms_vendor_code, b_pad);
    }
};
static_assert(sizeof(MsOsStringDesc) == 18, "MS OS signature string fixed 18 bytes");

/// MS OS 1.0 Compatible ID feature descriptor header (fixed 16 bytes)
struct MsOsCompatIdHeader {
    std::uint32_t dw_length; // 16 + 24×number of function sections
    std::uint16_t bcd_version; // 0x0100
    std::uint16_t w_index; // MS_OS_COMPAT_ID_WINDEX
    std::uint8_t b_count; // Number of function sections
    std::array<std::uint8_t, 7> reserved;

    void append_to(data_type &d) const {
        vector_append_to_le(d, dw_length, bcd_version, w_index, b_count, reserved);
    }
};
static_assert(sizeof(MsOsCompatIdHeader) == 16, "MS OS Compatible ID header fixed 16 bytes");

/// MS OS 1.0 Compatible ID function section (fixed 24 bytes per interface)
struct MsOsCompatIdSection {
    std::uint8_t b_first_interface; // interface number (RNDIS control interface 0)
    std::uint8_t reserved1; // core fill_ext_compat fixed write 0x01
    std::array<std::uint8_t, 8> compatible_id; // "RNDIS", etc.; zero-pad if short
    std::array<std::uint8_t, 8> sub_compatible_id; // Full 0
    std::array<std::uint8_t, 6> reserved2; // Full 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, b_first_interface, reserved1, compatible_id, sub_compatible_id, reserved2);
    }
};
static_assert(sizeof(MsOsCompatIdSection) == 24, "MS OS Compatible ID function section fixed 24 bytes");
#pragma pack(pop)
} // namespace usbipdcpp
