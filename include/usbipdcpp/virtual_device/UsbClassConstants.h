#pragma once

#include <cstdint>

#include "usbipdcpp/constant.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {

// ==================== USB class-specific control request codes ====================
// Common to video/audio and other device classes (UVC 1.5 §4.2 / UAC 1.0 §5.2.3)
constexpr std::uint8_t RC_UNDEFINED = 0x00;
constexpr std::uint8_t SET_CUR = 0x01;
constexpr std::uint8_t SET_CUR_ALL = 0x11;
constexpr std::uint8_t GET_CUR = 0x81;
constexpr std::uint8_t GET_MIN = 0x82;
constexpr std::uint8_t GET_MAX = 0x83;
constexpr std::uint8_t GET_RES = 0x84;
constexpr std::uint8_t GET_LEN = 0x85; // Video class naming; called GET_MEM in the Audio specification
constexpr std::uint8_t GET_INFO = 0x86;
constexpr std::uint8_t GET_DEF = 0x87;

// ==================== USB class-specific descriptor types ====================
constexpr std::uint8_t CS_INTERFACE = 0x24;
constexpr std::uint8_t CS_ENDPOINT = 0x25;

// ==================== Standard USB descriptor structs ====================
// Fixed-length descriptors from the USB 2.0 specification. Field layout is mandated by the specification;
// represent with packed structs and check sizeof with static_assert to prevent field additions/removals from violating the specification.
// append_to is serialized by field (multi-byte fields are automatically converted to little-endian), regardless of platform byte order.

#pragma pack(push, 1)
/// Device descriptor (USB 2.0 Table 9-8, fixed 18 bytes)
struct DeviceDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t bcdUSB;
    std::uint8_t bDeviceClass;
    std::uint8_t bDeviceSubClass;
    std::uint8_t bDeviceProtocol;
    std::uint8_t bMaxPacketSize0;
    std::uint16_t idVendor;
    std::uint16_t idProduct;
    std::uint16_t bcdDevice;
    std::uint8_t iManufacturer;
    std::uint8_t iProduct;
    std::uint8_t iSerialNumber;
    std::uint8_t bNumConfigurations;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bcdUSB, bDeviceClass, bDeviceSubClass, bDeviceProtocol,
                            bMaxPacketSize0, idVendor, idProduct, bcdDevice, iManufacturer, iProduct, iSerialNumber,
                            bNumConfigurations);
    }
};
static_assert(sizeof(DeviceDesc) == 18, "Device descriptor is fixed at 18 bytes");

/// Configuration descriptor (USB 2.0 Table 9-10, fixed 9 bytes)
struct ConfigHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumInterfaces;
    std::uint8_t bConfigurationValue;
    std::uint8_t iConfiguration;
    std::uint8_t bmAttributes;
    std::uint8_t bMaxPower;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, wTotalLength, bNumInterfaces, bConfigurationValue,
                            iConfiguration, bmAttributes, bMaxPower);
    }
};
static_assert(sizeof(ConfigHeaderDesc) == 9, "Configuration descriptor is fixed at 9 bytes");

/// Interface Association Descriptor, IAD (USB 2.0 ECN, fixed 8 bytes)
struct IadDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bFirstInterface;
    std::uint8_t bInterfaceCount;
    std::uint8_t bFunctionClass;
    std::uint8_t bFunctionSubClass;
    std::uint8_t bFunctionProtocol;
    std::uint8_t iFunction;

    /// Convenience construction: automatically populate bLength/bDescriptorType/bFirstInterface (bFirstInterface
    /// is filled by configuration descriptor generation from the owning interface's interface_number; interface numbers
    /// need not be considered at declaration; see UsbInterface::interface_association_descriptor)
    static IadDesc make(std::uint8_t b_interface_count, std::uint8_t b_function_class,
                        std::uint8_t b_function_subclass, std::uint8_t b_function_protocol = 0,
                        std::uint8_t i_function = 0) {
        return IadDesc{0x08, static_cast<std::uint8_t>(DescriptorType::InterfaceAssociation),
                       0, // bFirstInterface: filled during generation
                       b_interface_count, b_function_class, b_function_subclass, b_function_protocol, i_function};
    }

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bFirstInterface, bInterfaceCount, bFunctionClass,
                            bFunctionSubClass, bFunctionProtocol, iFunction);
    }
};
static_assert(sizeof(IadDesc) == 8, "IAD descriptor is fixed at 8 bytes");

/// Interface descriptor (USB 2.0 Table 9-12, fixed 9 bytes)
struct InterfaceDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bInterfaceNumber;
    std::uint8_t bAlternateSetting;
    std::uint8_t bNumEndpoints;
    std::uint8_t bInterfaceClass;
    std::uint8_t bInterfaceSubClass;
    std::uint8_t bInterfaceProtocol;
    std::uint8_t iInterface;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints,
                            bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface);
    }
};
static_assert(sizeof(InterfaceDesc) == 9, "Interface descriptor is fixed at 9 bytes");

/// Endpoint descriptor (USB 2.0 Table 9-13, fixed 7 bytes)
/// Note: audio10.pdf Table 4-20 requires 9 bytes for audio ISO endpoints (bRefresh/bSynchAddress),
/// but usbip-win2 uses the UDE framework, and Udecx.sys parses endpoint descriptors as 7 bytes; 9 bytes would misalign
/// subsequent descriptors and fail Windows enumeration, so retain 7 bytes (do not extend)
struct EndpointDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bEndpointAddress;
    std::uint8_t bmAttributes;
    std::uint16_t wMaxPacketSize;
    std::uint8_t bInterval;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bEndpointAddress, bmAttributes, wMaxPacketSize, bInterval);
    }
};
static_assert(sizeof(EndpointDesc) == 7, "Endpoint descriptor is fixed at 7 bytes");

/// BOS descriptor header (USB 3.0 Table 9-14, compatible with USB 2.0 devices, fixed 5 bytes)
struct BosHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumCapabilities;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, wTotalLength, bNumCapabilities);
    }
};
static_assert(sizeof(BosHeaderDesc) == 5, "BOS header is fixed at 5 bytes");

/// USB 2.0 Extension device capability descriptor (fixed 7 bytes)
struct BosUsb20ExtCapDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDevCapabilityType;
    std::uint32_t bmAttributes;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDevCapabilityType, bmAttributes);
    }
};
static_assert(sizeof(BosUsb20ExtCapDesc) == 7, "USB 2.0 Extension capability descriptor is fixed at 7 bytes");
#pragma pack(pop)

} // namespace usbipdcpp
