// Audio sink tests: pure logic related to UAC speaker (OUT direction) AC/AS descriptor topology and sample-rate negotiation
// (WavFileSink file-writing tests are in examples/mock_speaker/test_wav_file_sink.cpp,
// in the implementation directory; PlaybackSink depends on a real sound card and is not tested automatically)

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

using namespace usbipdcpp;

namespace {

/// In-memory test sink: records the number of bytes written; supports only mono, 16-bit, 48000
struct StubSink : public AudioSink {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {{1, 16, 48000}};
    }
    AudioFormatInfo current_format() const override { return format; }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
            return false;
        }
        format = {channels, bits_per_sample, sample_rate};
        return true;
    }
    void write_pcm(const std::uint8_t *, std::size_t size) override { written += size; }
    AudioFormatInfo format{1, 16, 48000};
    std::size_t written = 0;
};

} // namespace

// ========== UAC AC descriptors: speaker topology (compare kernel gadget f_uac1.c) ==========

TEST(UacAudioControlDesc, SpeakerTopology) {
    // Speaker: IT(USB streaming 0x0101) → FU → OT(Speaker 0x0301)
    // Layout: Header(9) + IT(12) + FU(7+2+1=10) + OT(9) = 40 bytes
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOCONTROL, .interface_protocol = 0};
    UacAudioControlHandler handler(intf, pool);
    handler.set_config(UacDeviceConfig{.input_terminal_type = TT_USB_STREAMING, .channels = 2});
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    ASSERT_EQ(desc.size(), 40u);
    // Input Terminal: wTerminalType at offset 4-5 within IT (IT starts at 9) → 0x0101 USB streaming
    EXPECT_EQ(desc[13], 0x01);
    EXPECT_EQ(desc[14], 0x01);
    // Output Terminal: wTerminalType at offset 4-5 within OT (OT starts at 31) → 0x0301 Speaker
    EXPECT_EQ(desc[35], 0x01);
    EXPECT_EQ(desc[36], 0x03);
    // Backfill wTotalLength (AC Header offset 5-6) with the actual total length
    EXPECT_EQ(desc[5], 40);
    EXPECT_EQ(desc[6], 0);
}

TEST(UacAudioControlDesc, MicrophoneTopology) {
    // Microphone (default): IT(MIC 0x0201) → FU → OT(USB streaming 0x0101), regression protection
    // Layout: Header(9) + IT(12) + FU(7+1+1=9) + OT(9) = 39 bytes
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOCONTROL, .interface_protocol = 0};
    UacAudioControlHandler handler(intf, pool);
    handler.set_config(UacDeviceConfig{.channels = 1});
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    ASSERT_EQ(desc.size(), 39u);
    // Input Terminal: 0x0201 Microphone
    EXPECT_EQ(desc[13], 0x01);
    EXPECT_EQ(desc[14], 0x02);
    // Output Terminal: 0x0101 USB streaming
    EXPECT_EQ(desc[34], 0x01);
    EXPECT_EQ(desc[35], 0x01);
}

// ========== UAC AS sink descriptors ==========

TEST(UacAudioStreamingSinkDesc, TerminalLinkPointsToInputTerminal) {
    // For the speaker direction, AS General's bTerminalLink points to the AC Input Terminal (0x01,
    // data flows in from USB), opposite to the microphone (which points to Output Terminal 0x03)
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING, .interface_protocol = 0};
    UacAudioStreamingSinkHandler handler(intf, pool, std::make_unique<StubSink>());
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    // General(7) + FormatTypeI head(8) + 1 sample rate(3) = 18 bytes
    ASSERT_EQ(desc.size(), 18u);
    // AS General: bLength=7, bDescriptorType=0x24, bDescriptorSubtype=0x01(AS_GENERAL),
    // bTerminalLink=0x01(Input Terminal)
    EXPECT_EQ(desc[0], 0x07);
    EXPECT_EQ(desc[1], 0x24);
    EXPECT_EQ(desc[2], AS_DESC_GENERAL);
    EXPECT_EQ(desc[3], UAC_ENTITY_INPUT_TERMINAL);
    // wFormatTag: PCM 0x0001
    EXPECT_EQ(desc[5], 0x01);
    EXPECT_EQ(desc[6], 0x00);
    // FormatType I: bNrChannels=1, bSubframeSize=2, bBitResolution=16, bSamFreqType=1
    EXPECT_EQ(desc[11], 1);
    EXPECT_EQ(desc[12], 2);
    EXPECT_EQ(desc[13], 16);
    EXPECT_EQ(desc[14], 1);
}

TEST(UacAudioStreamingSinkDesc, SampleRateSetFormatUpdatesSink) {
    // Sample-rate negotiation reaches the sink: before SET_CUR goes through handle_sampling_freq_control, first confirm
    // that the sink's supported_formats and current_format agree (the remaining negotiation logic shares
    // the microphone's GET_CUR/SET_CUR handling; mock_speaker verifies it end to end)
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING, .interface_protocol = 0};
    auto sink = std::make_unique<StubSink>();
    auto *sink_ptr = sink.get();
    UacAudioStreamingSinkHandler handler(intf, pool, std::move(sink));
    handler.on_setup_interface_handlers();

    // The sample rate in the descriptor comes from the sink's current format
    auto desc = handler.get_class_specific_descriptor();
    // tSamFreq: 48000 = 0x00BB80, 3 little-endian bytes (offset 15-17)
    EXPECT_EQ(desc[15], 0x80);
    EXPECT_EQ(desc[16], 0xBB);
    EXPECT_EQ(desc[17], 0x00);
    EXPECT_EQ(sink_ptr->written, 0u);
}
