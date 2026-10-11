#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {
// ECM (CDC Ethernet Networking Control Model) class-specific constants and descriptor structures.
// refers to the ECM 1.2 specification (ECM120) and the kernel f_ecm.c; the Header/Union function descriptor is
// CDC general definition, directly reuse the structure in CdcAcmConstants.h

// ECM class specific request code (ECM120 §6.2 table 6)
enum class EcmRequest {
    SetEthernetMulticastFilters = 0x40,
    SetEthernetPowerManagementPatternFilter = 0x41,
    GetEthernetPowerManagementPatternFilter = 0x42,
    SetEthernetPacketFilter = 0x43,
    GetEthernetStatistic = 0x44,
};

// ECM class specific notification code (ECM120 §6.3 table 11)
enum class EcmNotification {
    NetworkConnection = 0x00,
    ResponseAvailable = 0x01,
    ConnectionSpeedChange = 0x2A,
};

// ==================== Fixed-length class-specific descriptor structure ====================
// The length of  is rigidly specified by the specification and is represented by a packed structure. static_assert checks sizeof.
// append_to is serialized by field (multi-byte fields are automatically converted to little-endian), regardless of platform byte order.

#pragma pack(push, 1)
/// Ethernet Networking function descriptor (ECM120 §5.4 table 3, fixed 13 bytes)
struct CdcEthernetFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype; // 0x0F EthernetNetworking
    std::uint8_t iMACAddress; // MAC address string index (cannot be 0, bind will fail if cdc_ether cannot be read by the host driver)
    std::uint32_t bmEthernetStatistics; // Full 0 = GetEthernetStatistic not supported
    std::uint16_t wMaxSegmentSize; // Maximum Ethernet frame length, typical 1514
    std::uint16_t wNumberMCFilters; // multicast filter number, D15=0 perfect filtering
    std::uint8_t bNumberPowerFilters; // Power management mode filter number (0 = not supported)

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, iMACAddress, bmEthernetStatistics,
                            wMaxSegmentSize, wNumberMCFilters, bNumberPowerFilters);
    }
};
static_assert(sizeof(CdcEthernetFunctionalDesc) == 13, "Ethernet Networking function descriptor must be 13 bytes");

/// CDC notification header (USB CDC 1.2 §6.3, fixed 8 bytes)
/// Status notification puts the status in wValue (such as 0/1 of NETWORK_CONNECTION), and the data field is optional
struct CdcNotificationHeader {
    std::uint8_t bmRequestType; // 0xA1: Class|Interface|IN
    std::uint8_t bNotificationCode;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bmRequestType, bNotificationCode, wValue, wIndex, wLength);
    }
};
static_assert(sizeof(CdcNotificationHeader) == 8, "CDC notification header fixed 8 bytes");
#pragma pack(pop)

/// Assemble an interrupt IN notification message (8 byte header + optional data), return the complete message bytes
inline data_type make_cdc_notification(EcmNotification code, std::uint16_t w_value, std::uint16_t w_index,
                                       const std::uint8_t *data, std::uint16_t length) {
    data_type bytes;
    CdcNotificationHeader{0xA1, static_cast<std::uint8_t>(code), w_value, w_index, length}.append_to(bytes);
    if (length > 0 && data != nullptr) {
        bytes.insert(bytes.end(), data, data + length);
    }
    return bytes;
}
} // namespace usbipdcpp
