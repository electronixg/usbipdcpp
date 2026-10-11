// UVC video-streaming interface (VS) data-plane tests: iso IN frame retrieval/packetization, frame-clock empty-packet gating,
// stream-state (SET_INTERFACE + VS_COMMIT) control. Control plane (descriptors, PROBE negotiation)
// outside this file

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// Test video source with fixed frame size (YUY2 640x480, frame interval 1/30s = 333333×100ns).
// frame_size configurable (test large frames across URBs); fail_next simulates frame retrieval failure
struct StubSource : VideoSource {
    explicit StubSource(std::size_t frame_size = 64) : data(frame_size) {
        for (std::size_t i = 0; i < data.size(); i++) {
            data[i] = static_cast<std::uint8_t>(i);
        }
    }

    std::vector<VideoFormatInfo> supported_formats() const override {
        return {{.fourcc = 0x34363259, // YUY2
                 .width = 640,
                 .height = 480,
                 .max_frame_size = 64,
                 .default_frame_interval = 333333,
                 .min_frame_interval = 333333,
                 .max_frame_interval = 400000,
                 .bits_per_pixel = 16}};
    }
    VideoFormatInfo current_format() const override { return fmt; }
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override {
        fmt = {.fourcc = fourcc,
               .width = width,
               .height = height,
               .max_frame_size = 64,
               .default_frame_interval = frame_interval,
               .min_frame_interval = 333333,
               .max_frame_interval = 400000,
               .bits_per_pixel = 16};
        interval = frame_interval;
        return true;
    }
    bool get_frame(VideoFrame &frame) override {
        if (fail_next) {
            fail_next = false;
            return false;
        }
        frame = {data.data(), data.size(), true};
        return true;
    }
    std::size_t max_frame_size() const override { return data.size(); }
    std::uint32_t frame_interval() const override { return interval; }

    VideoFormatInfo fmt{0x34363259, 640, 480, 64, 333333, 333333, 400000, 16};
    std::uint32_t interval = 333333;
    data_type data;
    bool fail_next = false;
};

struct UvcTestEnv {
    StringPool pool;
    // Transfer allocator: must be declared before stub (members destroyed in reverse order; op dies later)—
    // RetSubmit entries in submits hold TransferHandle; destruction uses op to release it
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = 0x0E, .interface_subclass = 0x02, .interface_protocol = 0x00};
    StubSource *source = nullptr;
    UvcVideoStreamingHandler handler{intf, pool, std::unique_ptr<StubSource>()};
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    explicit UvcTestEnv(std::size_t frame_size = 64) :
        source(new StubSource(frame_size)), handler(intf, pool, std::unique_ptr<StubSource>(source)) {
        handler.on_new_connection(stub, ec);
    }

    // Follow Linux driver's stream-start order: SET_INTERFACE(1) → VS_COMMIT(SET_CUR) →
    // SET_INTERFACE(1) (streaming actually starts on second call)
    void start_streaming() {
        std::uint32_t status = 0;
        handler.request_set_interface(1, &status);
        EXPECT_EQ(status, 0u);

        // VS_COMMIT data: bFormatIndex=1, bFrameIndex=1, dwFrameInterval=333333
        data_type commit(26, 0);
        commit[2] = 1;
        commit[3] = 1;
        commit[4] = 0x15; // 333333 = 0x00051615 little-endian
        commit[5] = 0x16;
        commit[6] = 0x05;
        const SetupPacket setup{
                .request_type = 0x21, // Class | Interface | OUT
                .request = 0x01,      // SET_CUR
                .value = static_cast<std::uint16_t>(VS_COMMIT_CONTROL << 8), // wValue high byte = control selector
                .index = 0,
                .length = 26,
        };
        handler.handle_non_standard_request_type_control_urb(
                1, UsbEndpoint::get_ep0_in(UsbSpeed::High), 0, 26, setup,
                make_cmd_submit(op, 1, 0x00, UsbIpDirection::Out, 26, setup, commit).transfer, ec);
        EXPECT_FALSE(ec);
        EXPECT_EQ(stub.submits.back().header.seqnum, 1u); // commit ack

        handler.request_set_interface(1, &status);
        EXPECT_EQ(status, 0u);
    }

    // Send one iso IN URB (default 4 packets × 512 bytes)
    void deliver_iso_in(std::uint32_t seqnum, int num_packets = 4) {
        auto submit = make_cmd_submit(op, seqnum, 0x01, UsbIpDirection::In, num_packets * 512, {}, {}, num_packets);
        auto *trx = GenericTransfer::from_handle(submit.transfer.get());
        for (int i = 0; i < num_packets; i++) {
            trx->iso_descriptors[i] = {.offset = static_cast<std::uint32_t>(i * 512),
                                       .length = 512,
                                       .actual_length = 0,
                                       .status = 0};
        }
        handler.handle_isochronous_transfer(seqnum,
                                            UsbEndpoint{.address = 0x81, .attributes = 0x05, .max_packet_size = 512},
                                            0, num_packets * 512, std::move(submit.transfer), num_packets, ec);
        EXPECT_FALSE(ec);
    }
};

} // namespace

TEST(TestUvcHandler, IsoInBeforeCommitStalls) {
    // Before streaming (no SET_INTERFACE / no COMMIT), iso IN returns EPIPE
    UvcTestEnv env;

    env.deliver_iso_in(2);

    ASSERT_GE(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(env.stub.submits.back().actual_length, 0u);
}

TEST(TestUvcHandler, IsoInServesFrameWithHeaders) {
    // After streaming starts, iso IN retrieves and packetizes source frame: each packet has 2-byte payload header + data,
    // first packet has FID bit, last has EOF bit (whole frame in one packet); iso descriptor status cleared
    UvcTestEnv env;
    env.start_streaming();

    env.deliver_iso_in(2);

    // Response: seqnum 2, total sent = 2-byte header + 64-byte frame
    const auto *ret = &env.stub.submits.back();
    ASSERT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 66u);
    EXPECT_EQ(ret->number_of_packets, 4u);

    auto *trx = GenericTransfer::from_handle(ret->transfer.get());
    ASSERT_EQ(trx->iso_descriptors.size(), 4u);
    // First packet: header(2) + entire frame data, FID(0x01) + EOF(0x80)
    EXPECT_EQ(trx->iso_descriptors[0].actual_length, 66u);
    EXPECT_EQ(trx->iso_descriptors[0].status, 0); // Initially -EXDEV, now cleared
    EXPECT_EQ(trx->data[0], UVC_PAYLOAD_HEADER_SIZE);
    EXPECT_EQ(trx->data[1], UVC_PAYLOAD_HEADER_FID | UVC_PAYLOAD_HEADER_EOF);
    data_type expected_frame(64);
    for (std::size_t i = 0; i < 64; i++) {
        expected_frame[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(data_type(trx->data.begin() + UVC_PAYLOAD_HEADER_SIZE, trx->data.begin() + 66), expected_frame);
    // Other packets have no data
    for (int i = 1; i < 4; i++) {
        EXPECT_EQ(trx->iso_descriptors[i].actual_length, 0u);
        EXPECT_EQ(trx->iso_descriptors[i].status, 0);
    }
}

TEST(TestUvcHandler, FrameIntervalGateEmitsEmptyPacket) {
    // Immediately issue another iso IN after first frame completes: frame interval (33ms) not elapsed → empty packet (actual_length=0),
    // avoiding accelerated playback when host pulls quickly
    UvcTestEnv env;
    env.start_streaming();

    env.deliver_iso_in(2); // First frame sent
    env.deliver_iso_in(3); // Second frame immediately: frame interval not elapsed

    ASSERT_GE(env.stub.submits.size(), 2u);
    const auto *ret = &env.stub.submits.back();
    ASSERT_EQ(ret->header.seqnum, 3u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 0u); // Empty packet
}

TEST(TestUvcHandler, LargeFrameSpansMultipleUrbs) {
    // Large frame (3000 bytes) does not fit one URB: continue across multiple URBs (accumulate frame_offset_),
    // skip frame-clock check while frame incomplete; EOF appears only in final segment of last packet; data from URBs
    // concatenates to entire frame
    UvcTestEnv env(3000);
    env.start_streaming();

    // 2-packet URB capacity = 2×510 = 1020 bytes/operation; 3000 = 1020 + 1020 + 960
    env.deliver_iso_in(2, 2);
    env.deliver_iso_in(3, 2);
    env.deliver_iso_in(4, 2);

    // Data from three URBs concatenated in order = entire frame
    data_type assembled;
    std::size_t eof_found_at = 0;
    for (std::uint32_t seq : {2u, 3u, 4u}) {
        const auto *ret = &env.stub.submits[seq - 1]; // submits[0] is commit ack
        ASSERT_EQ(ret->header.seqnum, seq);
        EXPECT_EQ(ret->status, 0u);
        auto *trx = GenericTransfer::from_handle(ret->transfer.get());
        // Two packets, each with 2-byte header + data
        ASSERT_EQ(trx->iso_descriptors.size(), 2u);
        for (auto &iso : trx->iso_descriptors) {
            ASSERT_GE(iso.actual_length, UVC_PAYLOAD_HEADER_SIZE);
            auto &d = trx->data;
            if (d[static_cast<std::size_t>(iso.offset) + 1] & UVC_PAYLOAD_HEADER_EOF) {
                eof_found_at = assembled.size() + iso.actual_length - UVC_PAYLOAD_HEADER_SIZE;
            }
            assembled.insert(assembled.end(), d.begin() + iso.offset + UVC_PAYLOAD_HEADER_SIZE,
                             d.begin() + iso.offset + iso.actual_length);
        }
    }
    EXPECT_EQ(assembled.size(), 3000u);
    EXPECT_EQ(eof_found_at, 3000u); // EOF only at whole-frame end

    data_type expected(3000);
    for (std::size_t i = 0; i < expected.size(); i++) {
        expected[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(assembled, expected);
}

TEST(TestUvcHandler, GetFrameFailureStalls) {
    // Source frame retrieval fails (get_frame returns false): iso IN returns EPIPE (driver retries)
    UvcTestEnv env;
    env.source->fail_next = true;
    env.start_streaming();

    env.deliver_iso_in(2);

    ASSERT_GE(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestUvcHandler, ShortCommitFallsBackToDefaults) {
    // VS_COMMIT data shorter than 26 bytes (minimum parsing length): do not reject (tolerate incomplete host
    // submissions); retain default format (probe_data_ initial value), streaming still starts normally
    UvcTestEnv env;

    std::uint32_t status = 0;
    env.handler.request_set_interface(1, &status);
    EXPECT_EQ(status, 0u);

    data_type commit(10, 0); // Too short
    const SetupPacket setup{
            .request_type = 0x21,
            .request = 0x01,
            .value = static_cast<std::uint16_t>(VS_COMMIT_CONTROL << 8),
            .index = 0,
            .length = 10,
    };
    env.handler.handle_non_standard_request_type_control_urb(
            1, UsbEndpoint::get_ep0_in(UsbSpeed::High), 0, 10, setup,
            make_cmd_submit(env.op, 1, 0x00, UsbIpDirection::Out, 10, setup, commit).transfer, env.ec);
    EXPECT_FALSE(env.ec);
    EXPECT_EQ(env.stub.submits.back().status, 0u); // commit ack successful

    env.handler.request_set_interface(1, &status);
    env.deliver_iso_in(2);

    // Stream retrieves frames normally (default format)
    ASSERT_GE(env.stub.submits.size(), 2u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, 0u);
    EXPECT_GT(env.stub.submits.back().actual_length, 0u);
}
