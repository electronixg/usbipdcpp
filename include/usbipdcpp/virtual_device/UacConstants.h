#pragma once

#include <bit>
#include <cstdint>

// general class request code (SET_CUR/GET_CUR, etc.) and class-specific descriptor type (CS_INTERFACE, etc.)
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {

// ==================== USB Audio Class codes ====================
constexpr std::uint8_t CC_AUDIO = 0x01;
constexpr std::uint8_t SC_AUDIOCONTROL = 0x01;
constexpr std::uint8_t SC_AUDIOSTREAMING = 0x02;

// ==================== AC Interface Descriptor Subtypes ====================
constexpr std::uint8_t AC_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t AC_DESC_HEADER = 0x01;
constexpr std::uint8_t AC_DESC_INPUT_TERMINAL = 0x02;
constexpr std::uint8_t AC_DESC_OUTPUT_TERMINAL = 0x03;
constexpr std::uint8_t AC_DESC_MIXER_UNIT = 0x04;
constexpr std::uint8_t AC_DESC_SELECTOR_UNIT = 0x05;
constexpr std::uint8_t AC_DESC_FEATURE_UNIT = 0x06;

// ==================== AS Interface Descriptor Subtypes ====================
constexpr std::uint8_t AS_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t AS_DESC_GENERAL = 0x01;
constexpr std::uint8_t AS_DESC_FORMAT_TYPE = 0x02;

// ==================== AS class-specific endpoint descriptor ====================
// Linux snd-usb-audio relies on the SamplingFreqControl bit of bmAttributes to determine whether
// supports sampling rate negotiation; if missing, the driver skips SET_CUR and directly opens the stream at the requested rate.
constexpr std::uint8_t AS_EP_DESC_GENERAL = 0x01; // bDescriptorSubtype: EP_GENERAL
constexpr std::uint8_t AS_EP_ATTR_SAMPLING_FREQ = 0x01; // bmAttributes: SamplingFreqControl
constexpr std::uint8_t AS_EP_DESC_GENERAL_LEN = 7; // class-specific ISO audio data endpoint descriptor

// ==================== Terminal Types ====================
constexpr std::uint16_t TT_USB_STREAMING = 0x0101;
constexpr std::uint16_t ITT_MICROPHONE = 0x0201;
constexpr std::uint16_t ITT_SPEAKER = 0x0301;

// ==================== Channel Config ====================
constexpr std::uint16_t CHANNEL_CONFIG_MONO = 0x0004; // LEFT_FRONT
constexpr std::uint16_t CHANNEL_CONFIG_STEREO = 0x0003; // LEFT_FRONT | RIGHT_FRONT

// ==================== Entity IDs ====================
// prefix UAC_ to avoid duplicate names with ENTITY_* constants in UvcConstants.h
constexpr std::uint8_t UAC_ENTITY_INPUT_TERMINAL = 0x01;
constexpr std::uint8_t UAC_ENTITY_FEATURE_UNIT = 0x02;
constexpr std::uint8_t UAC_ENTITY_OUTPUT_TERMINAL = 0x03;

// ==================== Feature Unit Control Selectors ====================
constexpr std::uint8_t FU_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t FU_MUTE_CONTROL = 0x01;
constexpr std::uint8_t FU_VOLUME_CONTROL = 0x02;

// ==================== AS Interface Control Selectors ====================
constexpr std::uint8_t AS_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t AS_SAMPLING_FREQ_CONTROL = 0x01;

// ==================== UAC1 status word (AC interface interrupt endpoint) ====================
// UAC1_STATUS_TYPE_* of aligned kernel include/uapi/linux/usb/audio.h: status word 2 bytes
// (bStatusType + bOriginator), push via AC interrupt endpoint when FU control changes (audio_notify)
constexpr std::uint8_t UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF = 0x00;
constexpr std::uint8_t UAC1_STATUS_TYPE_IRQ_PENDING = 0x01;

// ==================== Audio format constants ====================
constexpr std::uint16_t AUDIO_FORMAT_PCM = 0x0001; // wFormatTag: PCM

// ==================== descriptor size constant ====================
constexpr std::uint8_t AC_HEADER_LEN = 9;
constexpr std::uint8_t AC_INPUT_TERMINAL_LEN = 12;
constexpr std::uint8_t AC_OUTPUT_TERMINAL_LEN = 9;
// Feature Unit Fixed number of bytes: bLength..bControlSize total 6 bytes + end iFeature 1 bytes.
// total length = fixed 7 + bmaControls bytes, where bmaControls bytes = number of channels + 1 (master + each logical channel)
constexpr std::uint8_t AC_FEATURE_UNIT_FIXED_LEN = 7;
constexpr std::uint8_t AS_GENERAL_LEN = 7;
constexpr std::uint8_t AS_FORMAT_TYPE_I_BASE_LEN = 8; // bLength..bSamFreqType total 8 bytes
constexpr std::uint8_t AS_SAMFREQ_ENTRY_LEN = 3; // Each sampling rate occupies 3 bytes (tSamFreq)

// ==================== Fixed length descriptor structure ====================
// UAC 1.0 descriptor length is rigidly specified by the specification and is represented by a packed structure.
// static_assert Verification sizeof prevents the addition and deletion of fields from causing the length to deviate from the specification.
// append_to is serialized by field (multi-byte fields are automatically converted to little-endian), regardless of platform byte order.

#pragma pack(push, 1)
/// AC Header descriptor (UAC 1.0 Table 4-2, single AS interface fixed 9 bytes)
struct AcHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t bcdADC;
    std::uint16_t wTotalLength;
    std::uint8_t bInCollection;
    std::uint8_t baInterfaceNr;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bcdADC, wTotalLength, bInCollection,
                            baInterfaceNr);
    }
};
static_assert(sizeof(AcHeaderDesc) == AC_HEADER_LEN, "AC Header descriptor must be 9 bytes");

/// Input Terminal descriptor (UAC 1.0 Table 4-3, fixed 12 bytes)
struct AcInputTerminalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalID;
    std::uint16_t wTerminalType;
    std::uint8_t bAssocTerminal;
    std::uint8_t bNrChannels;
    std::uint16_t wChannelConfig;
    std::uint8_t iChannelNames;
    std::uint8_t iTerminal;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalID, wTerminalType, bAssocTerminal,
                            bNrChannels, wChannelConfig, iChannelNames, iTerminal);
    }
};
static_assert(sizeof(AcInputTerminalDesc) == AC_INPUT_TERMINAL_LEN, "Input Terminal descriptor must be 12 bytes");

/// Output Terminal descriptor (UAC 1.0 Table 4-4, fixed 9 bytes)
struct AcOutputTerminalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalID;
    std::uint16_t wTerminalType;
    std::uint8_t bAssocTerminal;
    std::uint8_t bSourceID;
    std::uint8_t iTerminal;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalID, wTerminalType, bAssocTerminal,
                            bSourceID, iTerminal);
    }
};
static_assert(sizeof(AcOutputTerminalDesc) == AC_OUTPUT_TERMINAL_LEN, "Output Terminal descriptor must be 9 bytes");

/// Feature Unit descriptor fixed header (UAC 1.0 Table 4-7, bLength..bControlSize total 6 bytes)
struct AcFeatureUnitHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bUnitID;
    std::uint8_t bSourceID;
    std::uint8_t bControlSize;

    /// Add complete Feature Unit descriptor: fixed head + bmaControls[ch+1] (all channels share the same set of controls) + iFeature
    void append_to(data_type &d, std::uint8_t bma_controls, std::uint8_t channels) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bUnitID, bSourceID, bControlSize);
        for (int i = 0; i < channels + 1; ++i)
            d.push_back(bma_controls);
        d.push_back(0x00); // iFeature
    }
};
static_assert(sizeof(AcFeatureUnitHead) == 6, "Feature Unit fixed header must be 6 bytes");

/// AS General descriptor (UAC 1.0 Table 4-19, fixed 7 bytes)
struct AsGeneralDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalLink;
    std::uint8_t bDelay;
    std::uint16_t wFormatTag;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalLink, bDelay, wFormatTag);
    }
};
static_assert(sizeof(AsGeneralDesc) == AS_GENERAL_LEN, "AS General descriptor must be 7 bytes");

/// Format Type I descriptor fixed header (bLength..bSamFreqType total 8 bytes)
struct AsFormatTypeIHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bFormatType;
    std::uint8_t bNrChannels;
    std::uint8_t bSubframeSize;
    std::uint8_t bBitResolution;
    std::uint8_t bSamFreqType;

    /// Append complete Format Type I descriptor: fixed header + tSamFreq[3×n] (each item is 24 bit little-endian)
    void append_to(data_type &d, const std::vector<std::uint32_t> &rates) const {
        // reserves all variable parts at one time to avoid repeated expansion in the loop
        d.reserve(d.size() + sizeof(AsFormatTypeIHead) + rates.size() * AS_SAMFREQ_ENTRY_LEN);
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bFormatType, bNrChannels, bSubframeSize,
                            bBitResolution, bSamFreqType);
        for (auto rate: rates) {
            vector_append_to_le(d, static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                                static_cast<std::uint8_t>((rate >> 16) & 0xFF));
        }
    }
};
static_assert(sizeof(AsFormatTypeIHead) == AS_FORMAT_TYPE_I_BASE_LEN, "Format Type I fixed header must be 8 bytes");

/// EP_GENERAL class-specific endpoint descriptor (UAC 1.0 Table 4-21, fixed 7 bytes)
struct AsEpGeneralDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmAttributes;
    std::uint8_t bLockDelayUnits;
    std::uint16_t wLockDelay;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmAttributes, bLockDelayUnits, wLockDelay);
    }
};
static_assert(sizeof(AsEpGeneralDesc) == AS_EP_DESC_GENERAL_LEN, "EP_GENERAL endpoint descriptor must be 7 bytes");
#pragma pack(pop)

// ==================== UAC version ====================
constexpr std::uint16_t UAC_BCD_1_00 = 0x0100;

} // namespace usbipdcpp
