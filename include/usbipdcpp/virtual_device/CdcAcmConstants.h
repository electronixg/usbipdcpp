#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {
// CDC ACM class-specific descriptor subtype
enum class CdcAcmDescriptorSubtype {
    Header = 0x00,
    CallManagement = 0x01,
    ACM = 0x02,
    DirectLineManagement = 0x03,
    TelephoneRinger = 0x04,
    TelephoneCallStateReporting = 0x05,
    Union = 0x06,
    CountrySelection = 0x07,
    TelephoneOperatingModes = 0x08,
    USBTerminal = 0x09,
    NetworkChannelTerminal = 0x0A,
    ProtocolUnit = 0x0B,
    ExtensionUnit = 0x0C,
    MultiChannelManagement = 0x0D,
    CAPIControlManagement = 0x0E,
    EthernetNetworking = 0x0F,
    ATMNetworking = 0x10,
};

// CDC ACM control request code
enum class CdcAcmRequest {
    SendEncapsulatedCommand = 0x00,
    GetEncapsulatedResponse = 0x01,
    SetCommFeature = 0x02,
    GetCommFeature = 0x03,
    ClearCommFeature = 0x04,
    SetLineCoding = 0x20,
    GetLineCoding = 0x21,
    SetControlLineState = 0x22,
    SendBreak = 0x23,
};

// CDC ACM control signal bit
enum class CdcAcmControlSignal : std::uint16_t {
    DTR = 0x01, // Data Terminal Ready
    RTS = 0x02, // Request To Send
};

// CDC ACM serial port status bit
enum class CdcAcmSerialState : std::uint16_t {
    DCD = 0x01, // Data Carrier Detect
    DSR = 0x02, // Data Set Ready
    Break = 0x04, // Break signal
    Ring = 0x08, // Ring signal
    FramingError = 0x10, // Framing error
    ParityError = 0x20, // Parity error
    OverrunError = 0x40, // Overrun error
    CTS = 0x80, // Clear To Send
};

// ==================== Fixed-length class-specific descriptor structure ====================
// CDC 1.1 The length of the function descriptor is rigidly specified by the specification and is represented by a packed structure.
// static_assert Verification sizeof prevents the addition and deletion of fields from causing the length to deviate from the specification.
// append_to is serialized by field (multi-byte fields are automatically converted to little-endian), regardless of platform byte order.

#pragma pack(push, 1)
/// CDC Header function descriptor (CDC 1.1, fixed 5 bytes)
struct CdcHeaderFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t bcdCDC;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bcdCDC);
    }
};
static_assert(sizeof(CdcHeaderFunctionalDesc) == 5, "CDC Header function descriptor must be 5 bytes");

/// Call Management function descriptor (CDC 1.1, fixed 5 bytes)
struct CdcCallManagementDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmCapabilities;
    std::uint8_t bDataInterface;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmCapabilities, bDataInterface);
    }
};
static_assert(sizeof(CdcCallManagementDesc) == 5, "Call Management function descriptor must be 5 bytes");

/// ACM function descriptor (CDC 1.1, fixed 4 bytes)
struct CdcAcmFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmCapabilities;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmCapabilities);
    }
};
static_assert(sizeof(CdcAcmFunctionalDesc) == 4, "ACM function descriptor must be 4 bytes");

/// Union function descriptor (CDC 1.1, fixed 5 bytes, single slave interface)
struct CdcUnionFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bMasterInterface;
    std::uint8_t bSlaveInterface0;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bMasterInterface, bSlaveInterface0);
    }
};
static_assert(sizeof(CdcUnionFunctionalDesc) == 5, "Union function descriptor must be 5 bytes");
#pragma pack(pop)
} // namespace usbipdcpp
