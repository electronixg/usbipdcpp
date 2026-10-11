// UAC speaker (OUT stream reception) data-plane tests: TransferScheduler delays iso OUT transfers,
// then writes AudioSink and responds. Control plane (AC topology, sample-rate negotiation) in
// test_audio_sinks.cpp; this file tests only reception path and stream-state switching

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/// In-memory test sink: records written content; supports only mono, 16-bit, 48000.
/// Writer is TransferScheduler thread; test thread polls snapshots, requiring locking
struct RecordingSink : public AudioSink {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {{1, 16, 48000}};
    }
    AudioFormatInfo current_format() const override { return format; }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        if (bits_per_sample != 16 || channels != 1) {
            return false;
        }
        format = {channels, bits_per_sample, sample_rate};
        return true;
    }
    void write_pcm(const std::uint8_t *data, std::size_t size) override {
        std::lock_guard lock(mutex);
        pcm.insert(pcm.end(), data, data + size);
    }
    AudioFormatInfo format{1, 16, 48000};
    data_type snapshot() {
        std::lock_guard lock(mutex);
        return pcm;
    }
    std::mutex mutex;
    data_type pcm;
};

// Construct speaker device (matching examples/mock_speaker): AC (interrupt IN 0x82) +
// AS (alt0 empty / alt1 ISO OUT 0x01); setup_speaker registers handler and enables scheduler
struct SpeakerFixture {
    std::shared_ptr<UsbDevice> device;
    VirtualDeviceHandler *dh = nullptr;
    RecordingSink *sink = nullptr;
    UsbEndpoint ep_iso_out;
    // Transfer allocator: fixture member (fixture declared before test-local stub, destroyed afterward)—
    // RetSubmit entries in submits hold TransferHandle; destruction uses op to release it
    GenericTransferOperator op;
};

SpeakerFixture make_speaker_device(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x82, // IN, interrupt for status
                            .attributes = 0x03,
                            .max_packet_size = 2,
                            .interval = 4,
                    }}},
            },
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x01, // OUT, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Adaptive),
                                          .max_packet_size = 192,
                                          .interval = 4,
                                  }}},
            },
    };
    auto device = UsbDevice::make("/test/mock_speaker", 0x1234, 0x5683, std::move(interfaces), 1, 1, 0,
                                  "/usbipdcpp/mock_speaker", UsbSpeed::High, 0x0100);
    device->assign_interface_numbers();
    auto sink = std::make_unique<RecordingSink>();
    auto *sink_ptr = sink.get();
    // Assembly failures outside this test's scope (errors only when interface missing); retain error code for EXPECT fallback
    EXPECT_FALSE(UacDeviceHelper::setup_speaker(device, 0, string_pool, std::move(sink), {}));
    auto *dh = dynamic_cast<VirtualDeviceHandler *>(device->handler.get());
    return {device, dh, sink_ptr, device->interfaces[1].endpoints[1][0]};
}

// Send SET_INTERFACE control request to switch AS interface alt (streaming state-machine switch)
void set_as_interface(SpeakerFixture &fx, std::uint16_t alt, CaptureResponder &stub, usbipdcpp::error_code &ec) {
    const SetupPacket setup{
            .request_type = 0x01, // Host | Interface | OUT
            .request = 0x0B,      // SET_INTERFACE
            .value = alt,
            .index = 1, // AS interface number
            .length = 0,
    };
    fx.dh->receive_urb(make_cmd_submit(fx.op, 100, 0x00, UsbIpDirection::Out, 0, setup), fx.device->ep0_in,
                       std::nullopt, ec);
    ASSERT_FALSE(ec);
}

// Poll until scheduler thread consumes (iso OUT delayed according to data duration, millisecond scale)
void wait_for_sink_data(SpeakerFixture &fx, std::size_t expected,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (fx.sink->snapshot().size() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

TEST(TestUacSpeakerStream, IsoOutBeforeStreamingStalls) {
    // Without SET_INTERFACE (streaming=false, alt0 empty endpoints), iso OUT returns EPIPE
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    fx.dh->receive_urb(make_cmd_submit(fx.op, 1, 0x01, UsbIpDirection::Out, 384, {}, {}, 2), fx.ep_iso_out,
                       fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
    EXPECT_EQ(fx.sink->snapshot().size(), 0u); // Not streaming; sink consumes nothing

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, IsoOutWritesSinkThenResponds) {
    // After SET_INTERFACE alt=1: scheduler delays iso OUT according to data duration (4ms),
    // sink receives two compactly arranged PCM packets; response status 0, actual_length=total data volume,
    // iso descriptor status cleared (required by kernel driver)
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec); // alt 1: start stream

    // Two iso packets, 192 bytes each (48000Hz 16bit mono, 2ms), data compactly arranged
    data_type payload(384);
    for (std::size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 384, {}, payload, 2);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0};
    trx->iso_descriptors[1] = {.offset = 192, .length = 192, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // Scheduler thread writes sink and responds after data duration (384B / 96000B/s = 4ms)
    wait_for_sink_data(fx, payload.size());
    EXPECT_EQ(fx.sink->snapshot(), payload);

    // Read responses only after disconnection (join scheduler thread): process_iso_out writes submits on scheduler thread;
    // must stop thread first to establish happens-before, otherwise TSan reports data race
    fx.dh->on_disconnection(ec);

    // Find iso transfer (seqnum=2) in responses; SET_INTERFACE response seqnum=100
    ASSERT_EQ(stub.submits.size(), 2u);
    const auto *ret = &stub.submits[0];
    if (ret->header.seqnum != 2u) {
        ret = &stub.submits[1];
    }
    ASSERT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 384u);
    EXPECT_EQ(ret->number_of_packets, 2u);

    // iso descriptor status cleared (initially -EXDEV; without clearing, kernel considers packet failed),
    // actual_length filled with actual written amount
    auto *resp_trx = GenericTransfer::from_handle(ret->transfer.get());
    ASSERT_EQ(resp_trx->iso_descriptors.size(), 2u);
    EXPECT_EQ(resp_trx->iso_descriptors[0].status, 0);
    EXPECT_EQ(resp_trx->iso_descriptors[0].actual_length, 192u);
    EXPECT_EQ(resp_trx->iso_descriptors[1].status, 0);
    EXPECT_EQ(resp_trx->iso_descriptors[1].actual_length, 192u);
}

TEST(TestUacSpeakerStream, SwitchingBackToAlt0StopsStreaming) {
    // After switching back to alt0, streaming=false: iso OUT returns EPIPE again (SET_INTERFACE resets stream state)
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec);
    set_as_interface(fx, 0, stub, ec); // Stop stream

    fx.dh->receive_urb(make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 192, {}, {}, 1), fx.ep_iso_out,
                       fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // Final response is iso transfer (seqnum=2), EPIPE
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[2].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(fx.sink->snapshot().size(), 0u);

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, MuteZeroesReceivedPcm) {
    // After AC Feature Unit mute (SET_CUR(FU_MUTE)): reception side zeroes PCM before writing sink
    // (Windows volume-slider path); sink receives all zeros
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec); // alt 1: start stream

    // SET_CUR(FU_MUTE) to AC interface: wValue high byte = control selector (channel 0),
    // wIndex high byte = Feature Unit entity; 1-byte data = mute
    const SetupPacket mute_setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x01,      // SET_CUR
            .value = static_cast<std::uint16_t>(FU_MUTE_CONTROL << 8),
            .index = static_cast<std::uint16_t>(UAC_ENTITY_FEATURE_UNIT << 8),
            .length = 1,
    };
    fx.dh->receive_urb(make_cmd_submit(fx.op, 101, 0x00, UsbIpDirection::Out, 1, mute_setup, {1}),
                       fx.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);

    // Receive nonzero PCM stream
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 192, {}, data_type(192, 0xAB), 1);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    wait_for_sink_data(fx, 192);
    EXPECT_EQ(fx.sink->snapshot(), data_type(192, 0)); // All muted

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, DeclaredIsoLengthExceedsActualData) {
    // iso descriptor declares length greater than actual data (abnormal host/truncated packet): consume actual available data
    // (take = min(declared, remaining)); response actual_length = bytes actually received
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec);

    data_type payload(100, 0x11);
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 100, {}, payload, 1);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0}; // Declared 192 > actual 100
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    wait_for_sink_data(fx, 100);
    EXPECT_EQ(fx.sink->snapshot(), payload);

    fx.dh->on_disconnection(ec);
    const auto *ret = &stub.submits[0];
    if (ret->header.seqnum != 2u) {
        ret = &stub.submits[1];
    }
    EXPECT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->actual_length, 100u); // Actual bytes, not declared value
}
