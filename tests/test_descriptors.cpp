// Descriptor-struct serialization regression tests:
// Each struct's append_to output bytes must exactly match the specification and the construction code before the conversion to structs.
// Expected bytes are written independently of the implementation (against UAC 1.0 / UVC 1.5 / CDC 1.1 / USB 2.0 specifications),
// preventing regressions caused by refactoring that changes the wire byte sequence.

#include <gtest/gtest.h>

#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"

using namespace usbipdcpp;

// ==================== Basic serialization functions ====================

TEST(DescriptorStruct, HtoleLittleEndianPlatform) {
    // On little-endian platforms, htole returns the value unchanged; on big-endian platforms, it returns the byte-swapped result.
    // In both cases, the serialized bytes must match the little-endian wire format.
    if constexpr (std::endian::native == std::endian::little) {
        EXPECT_EQ(htole(std::uint16_t{0x1234}), 0x1234);
        EXPECT_EQ(htole(std::uint32_t{0x01020304}), 0x01020304);
    }
    else {
        EXPECT_EQ(htole(std::uint16_t{0x1234}), 0x3412);
        EXPECT_EQ(htole(std::uint32_t{0x01020304}), 0x04030201);
    }
}

TEST(DescriptorStruct, VectorAppendToLeAppendsToNonEmpty) {
    data_type d{0xAA, 0xBB};
    vector_append_to_le(d, std::uint16_t{0x1234}, std::uint8_t{0xCC});
    EXPECT_EQ(d, (data_type{0xAA, 0xBB, 0x34, 0x12, 0xCC}));
}

TEST(DescriptorStruct, VectorAppendToLeRangeParam) {
    data_type d;
    data_type payload{0xDE, 0xAD, 0xBE};
    vector_append_to_le(d, std::uint8_t{0x01}, payload, std::uint16_t{0x0102});
    EXPECT_EQ(d, (data_type{0x01, 0xDE, 0xAD, 0xBE, 0x02, 0x01}));
}

// ==================== Byte-level descriptor-struct regression ====================

TEST(DescriptorStruct, UacAcHeader) {
    data_type d;
    AcHeaderDesc{0x09, 0x24, AC_DESC_HEADER, UAC_BCD_1_00, 39, 0x01, 0x01}.append_to(d);
    EXPECT_EQ(d, (data_type{0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, 0x01}));
}

TEST(DescriptorStruct, UacInputTerminalMono) {
    data_type d;
    AcInputTerminalDesc{0x0C, 0x24, AC_DESC_INPUT_TERMINAL, UAC_ENTITY_INPUT_TERMINAL, ITT_MICROPHONE,
                        0x00, 1, CHANNEL_CONFIG_MONO, 0x00, 0x00}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00}));
}

TEST(DescriptorStruct, UacInputTerminalStereo) {
    data_type d;
    AcInputTerminalDesc{0x0C, 0x24, AC_DESC_INPUT_TERMINAL, UAC_ENTITY_INPUT_TERMINAL, ITT_MICROPHONE,
                        0x00, 2, CHANNEL_CONFIG_STEREO, 0x00, 0x00}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00}));
}

TEST(DescriptorStruct, UacFeatureUnitMono) {
    data_type d;
    // mute|volume = 0x03, mono → bmaControls[master+ch1] 1 byte each + iFeature
    AcFeatureUnitHead{0x09, 0x24, AC_DESC_FEATURE_UNIT, UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL, 0x01}
            .append_to(d, 0x03, 1);
    EXPECT_EQ(d, (data_type{0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x03, 0x00}));
}

TEST(DescriptorStruct, UacFeatureUnitStereo) {
    data_type d;
    // Stereo → bmaControls has 3 elements (master + ch1 + ch2)
    AcFeatureUnitHead{0x0A, 0x24, AC_DESC_FEATURE_UNIT, UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL, 0x01}
            .append_to(d, 0x03, 2);
    EXPECT_EQ(d, (data_type{0x0A, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x03, 0x03, 0x00}));
}

TEST(DescriptorStruct, UacOutputTerminal) {
    data_type d;
    AcOutputTerminalDesc{0x09, 0x24, AC_DESC_OUTPUT_TERMINAL, UAC_ENTITY_OUTPUT_TERMINAL, TT_USB_STREAMING,
                         0x00, UAC_ENTITY_FEATURE_UNIT, 0x00}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x00}));
}

TEST(DescriptorStruct, UacAsGeneral) {
    data_type d;
    AsGeneralDesc{0x07, 0x24, AS_DESC_GENERAL, UAC_ENTITY_OUTPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM}.append_to(d);
    EXPECT_EQ(d, (data_type{0x07, 0x24, 0x01, 0x03, 0x01, 0x01, 0x00}));
}

TEST(DescriptorStruct, UacAsFormatTypeI) {
    data_type d;
    AsFormatTypeIHead{17, 0x24, AS_DESC_FORMAT_TYPE, 0x01, 1, 0x02, 16, 3}
            .append_to(d, std::vector<std::uint32_t>{48000, 16000, 8000});
    // bLength = 8 + 3*3 = 17; all tSamFreq values are 24-bit little-endian
    EXPECT_EQ(d, (data_type{0x11, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x03,
                            0x80, 0xBB, 0x00, // 48000
                            0x80, 0x3E, 0x00, // 16000
                            0x40, 0x1F, 0x00})); // 8000
}

TEST(DescriptorStruct, UacAsEpGeneral) {
    data_type d;
    AsEpGeneralDesc{0x07, CS_ENDPOINT, AS_EP_DESC_GENERAL, AS_EP_ATTR_SAMPLING_FREQ, 0x00, 0x00}.append_to(d);
    EXPECT_EQ(d, (data_type{0x07, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00}));
}

TEST(DescriptorStruct, UvcVcHeader) {
    data_type d;
    VcHeaderDesc{0x0D, 0x24, VC_DESC_HEADER, UVC_BCD_1_50, 53, 0x019BFCC0, 0x01, 0x01}.append_to(d);
    EXPECT_EQ(d, (data_type{0x0D, 0x24, 0x01, 0x50, 0x01, 0x35, 0x00, 0xC0, 0xFC, 0x9B, 0x01, 0x01, 0x01}));
}

TEST(DescriptorStruct, UvcCameraTerminal) {
    data_type d;
    VcCameraTerminalDesc{0x12, 0x24, VC_DESC_INPUT_TERMINAL, 0x01, ITT_CAMERA, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x03, {0x00, 0x00, 0x00}}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x12, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}));
}

TEST(DescriptorStruct, UvcProcessingUnit) {
    data_type d;
    VcProcessingUnitDesc{0x0D, 0x24, VC_DESC_PROCESSING_UNIT, 0x02, 0x01, 0x00, 0x03, {0x1F, 0x02, 0x00}, 0x00, 0x00}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x0D, 0x24, 0x05, 0x02, 0x01, 0x00, 0x00, 0x03, 0x1F, 0x02, 0x00, 0x00, 0x00}));
}

TEST(DescriptorStruct, UvcOutputTerminal) {
    data_type d;
    VcOutputTerminalDesc{0x09, 0x24, VC_DESC_OUTPUT_TERMINAL, 0x03, TT_STREAMING, 0x00, 0x02, 0x00}.append_to(d);
    EXPECT_EQ(d, (data_type{0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x00}));
}

TEST(DescriptorStruct, UvcVsInputHeader) {
    data_type d;
    VsInputHeaderDesc{0x0E, 0x24, VS_DESC_INPUT_HEADER, 0x01, 0xABCD, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x0E, 0x24, 0x01, 0x01, 0xCD, 0xAB, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00}));
}

TEST(DescriptorStruct, UvcColorMatching) {
    data_type d;
    VsColorMatchingDesc{0x06, 0x24, VS_DESC_COLORFORMAT, VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709,
                        VIDEO_COLOR_COEF_SMPTE170M}
            .append_to(d);
    EXPECT_EQ(d, (data_type{0x06, 0x24, 0x0D, 0x01, 0x01, 0x04}));
}

TEST(DescriptorStruct, UvcInterruptEndpoint) {
    data_type d;
    VcInterruptEndpointDesc{0x05, CS_ENDPOINT, EP_INTERRUPT, 0x0040}.append_to(d);
    EXPECT_EQ(d, (data_type{0x05, 0x25, 0x03, 0x40, 0x00}));
}

TEST(DescriptorStruct, CdcHeader) {
    data_type d;
    CdcHeaderFunctionalDesc{0x05, 0x24, 0x00, 0x0110}.append_to(d);
    EXPECT_EQ(d, (data_type{0x05, 0x24, 0x00, 0x10, 0x01}));
}

TEST(DescriptorStruct, CdcCallManagement) {
    data_type d;
    CdcCallManagementDesc{0x05, 0x24, 0x01, 0x00, 0x01}.append_to(d);
    EXPECT_EQ(d, (data_type{0x05, 0x24, 0x01, 0x00, 0x01}));
}

TEST(DescriptorStruct, CdcAcm) {
    data_type d;
    CdcAcmFunctionalDesc{0x04, 0x24, 0x02, 0x02}.append_to(d);
    EXPECT_EQ(d, (data_type{0x04, 0x24, 0x02, 0x02}));
}

TEST(DescriptorStruct, CdcUnion) {
    data_type d;
    CdcUnionFunctionalDesc{0x05, 0x24, 0x06, 0x00, 0x01}.append_to(d);
    EXPECT_EQ(d, (data_type{0x05, 0x24, 0x06, 0x00, 0x01}));
}

TEST(DescriptorStruct, StdDevice) {
    data_type d;
    DeviceDesc{0x12, 0x01, 0x0200, 0x00, 0x00, 0x00, 64, 0x1234, 0x5682, 0x0100, 1, 2, 3, 1}.append_to(d);
    EXPECT_EQ(d, (data_type{0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x34,
                            0x12, 0x82, 0x56, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01}));
}

TEST(DescriptorStruct, StdConfigHeader) {
    data_type d;
    ConfigHeaderDesc{0x09, 0x02, 59, 2, 1, 0, 0x80, 0xFA}.append_to(d);
    EXPECT_EQ(d, (data_type{0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0x80, 0xFA}));
}

TEST(DescriptorStruct, StdIad) {
    data_type d;
    IadDesc{0x08, 0x0B, 0x00, 2, 0x0E, 0x03, 0x00, 0x02}.append_to(d);
    EXPECT_EQ(d, (data_type{0x08, 0x0B, 0x00, 0x02, 0x0E, 0x03, 0x00, 0x02}));
}

TEST(DescriptorStruct, StdInterface) {
    data_type d;
    InterfaceDesc{0x09, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0x00}.append_to(d);
    EXPECT_EQ(d, (data_type{0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00}));
}

TEST(DescriptorStruct, StdEndpoint) {
    data_type d;
    EndpointDesc{0x07, 0x05, 0x81, 0x05, 192, 1}.append_to(d);
    EXPECT_EQ(d, (data_type{0x07, 0x05, 0x81, 0x05, 0xC0, 0x00, 0x01}));
}

TEST(DescriptorStruct, StdBos) {
    data_type d;
    BosHeaderDesc{0x05, 0x0F, 12, 1}.append_to(d);
    BosUsb20ExtCapDesc{0x07, 0x10, 2, 0}.append_to(d);
    EXPECT_EQ(d, (data_type{0x05, 0x0F, 0x0C, 0x00, 0x01, 0x07, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00}));
}
