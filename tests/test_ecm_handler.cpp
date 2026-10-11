// ECM data-interface data-plane tests: bulk OUT frame reception (backend consumption / pending frame retrieval),
// bulk IN frame transmission (message mode: one frame per request). Control plane (descriptors, CDC requests) is outside this file

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/EcmVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// Test backend recording received frames
struct StubBackend : NetworkBackend {
    void send_frame(const std::uint8_t *data, std::size_t size) override {
        frames.emplace_back(data, data + size);
    }
    std::vector<data_type> frames;
};

// Subclass that does not consume host frames: pending OUT goes through take_frame (the channel remains pending without a backend and when on_frame_received
// returns false)
class HangingEcmHandler : public EcmDataInterfaceHandler {
public:
    using EcmDataInterfaceHandler::EcmDataInterfaceHandler;
    bool on_frame_received(data_type &&frame) override {
        return false;
    }
};

// Minimal usable ECM data interface (CDC Data class) + handler
struct EcmTestEnv {
    StringPool pool;
    // Transfer allocator: must be declared before stub (members destroyed in reverse order; op dies later)—
    // RetSubmit entries in submits hold TransferHandle; their destructors use op to release it
    GenericTransferOperator op;
    StubBackend backend;
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    EcmDataInterfaceHandler handler{intf, pool, &backend};
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    EcmTestEnv() : handler(intf, pool, &backend) {
        handler.on_new_connection(stub, ec);
    }
};

constexpr std::uint8_t EP_IN = 0x81;
constexpr std::uint8_t EP_OUT = 0x02;

} // namespace

TEST(TestEcmDataHandler, OutFrameDeliveredToBackendAndAcked) {
    // With backend: host OUT frames are passed directly to the network side for consumption and acknowledged immediately
    EcmTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                             0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                     0, frame.size(), make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out,
                                                                      frame.size(), {}, frame).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size());
}

TEST(TestEcmDataHandler, OutFrameHangsUntilTakeFrame) {
    // Without a consumer (subclass on_frame_received returns false): OUT stays pending (NAK backpressure),
    // acknowledged when the application thread retrieves it using take_frame
    StringPool pool;
    GenericTransferOperator op; // Declare before stub (TransferHandle destruction needs it)
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    HangingEcmHandler handler(intf, pool, nullptr);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const data_type frame = {'h', 'i', 0, 1, 2, 3};

    std::optional<OutEndpointChannel::Pending> taken;
    std::thread reader([&]() { taken = handler.take_frame(2000); });
    handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                 0, frame.size(), make_cmd_submit(op, 1, EP_OUT, UsbIpDirection::Out,
                                                                  frame.size(), {}, frame).transfer,
                                 ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ep, EP_OUT);
    EXPECT_EQ(taken->data, frame);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, frame.size());
}

TEST(TestEcmDataHandler, InRequestServesSentFrame) {
    // send_frame pushes one frame → host bulk IN request retrieves it (message mode: exactly one frame per URB)
    EcmTestEnv env;

    const data_type frame = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_EQ(env.handler.send_frame(frame), frame.size());

    env.handler.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, frame.size(), make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In,
                                                                      frame.size()).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size());
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), frame);
}

TEST(TestEcmDataHandler, InRequestHangsThenSendFrameServesIt) {
    // Host IN request first stays pending (no frame); respond directly when send_frame pushes one
    EcmTestEnv env;

    env.handler.handle_bulk_transfer(3, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 3, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // Pending, no response yet

    const data_type frame = {9, 8, 7, 6};
    env.handler.send_frame(frame);
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 3u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size()); // Frame shorter than request: terminated by a short packet
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), frame);
}

TEST(TestEcmDataHandler, UnlinkPendingInRequest) {
    // UNLINK cancels a pending IN request: return RET_UNLINK(-ECONNRESET), and send no further
    // RET_SUBMIT (request removed from the queue)
    EcmTestEnv env;

    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty());

    env.handler.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(env.stub.submits.empty());
}

TEST(TestEcmDataHandler, UnlinkPendingOutRequest) {
    // UNLINK cancels a pending OUT request (no consumer): return RET_UNLINK(-ECONNRESET)
    StringPool pool;
    GenericTransferOperator op; // Declare before stub (TransferHandle destruction needs it)
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    HangingEcmHandler handler(intf, pool, nullptr);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const data_type frame = {1, 2, 3};
    handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                 0, frame.size(), make_cmd_submit(op, 1, EP_OUT, UsbIpDirection::Out,
                                                                  frame.size(), {}, frame).transfer,
                                 ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(stub.submits.empty()); // Pending, no response yet

    handler.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(stub.submits.empty());
}

TEST(TestEcmDataHandler, TxBufferDropsOldestOverLimit) {
    // When the transmit-frame buffer exceeds the limit, drop the oldest frame (network data may be dropped; TCP retransmission provides recovery): send 3 frames with limit 2,
    // dropping frame 1; host IN requests can retrieve only the last two frames
    EcmTestEnv env;
    env.handler.set_tx_max_pending(2);

    env.handler.send_frame(data_type{1, 1, 1});
    env.handler.send_frame(data_type{2, 2, 2});
    env.handler.send_frame(data_type{3, 3, 3}); // Evict frame 1

    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    env.handler.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 2u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{2, 2, 2}));
    EXPECT_EQ(ret_submit_data(env.stub.submits[1]), (data_type{3, 3, 3}));

    // The third IN request stays pending (buffer now empty)
    env.handler.handle_bulk_transfer(3, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 3, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    EXPECT_EQ(env.stub.submits.size(), 2u);
}

TEST(TestEcmDataHandler, FrameLongerThanInRequestTruncated) {
    // Frame longer than host IN request: in message mode, truncate the response to the request length (consume the entire message,
    // discarding the remainder—host usbnet requests are normally ≥ MTU; short requests indicate an abnormal host)
    EcmTestEnv env;

    env.handler.send_frame(data_type{1, 2, 3, 4, 5, 6});
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 4, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 4).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].actual_length, 4u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{1, 2, 3, 4}));
}

TEST(TestEcmDataHandler, SendFrameAfterDisconnectDropsData) {
    // After disconnection, send_frame still returns the requested size (transparent to the caller), but the channel discards the data
    //(disconnection rejects enqueueing); reconnection starts with a clean buffer and no leftovers
    EcmTestEnv env;
    std::error_code disc_ec;
    env.handler.on_disconnection(disc_ec);

    EXPECT_EQ(env.handler.send_frame(data_type{1, 2, 3}), 3u);
    EXPECT_TRUE(env.stub.submits.empty());

    // After reconnection, IN request stays pending (buffer empty, no leftover data)
    std::error_code conn_ec;
    env.handler.on_new_connection(env.stub, conn_ec);
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // Pending, no response yet
}
