// RNDIS virtual network-adapter tests: control plane (encapsulated-command handshake, OID query/set, state machine) and
// data plane (RNDIS_MSG_PACKET encapsulation/decapsulation, multiple-packet aggregation, ZLP padding)

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/RndisConstants.h"
#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"
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

constexpr std::uint8_t EP_NOTIFY = 0x83;
constexpr std::uint8_t EP_IN = 0x81;
constexpr std::uint8_t EP_OUT = 0x02;
// Match default MAC in examples/mock_rndis
const std::array<std::uint8_t, 6> TEST_MAC = {0x02, 0x10, 0x83, 0x00, 0x00, 0x02};

// Minimal usable RNDIS dual interface (communication + data) + handler
struct RndisTestEnv {
    StringPool pool;
    // Transfer allocator: must be declared before stub (members destroyed in reverse order; op dies later)—
    // RetSubmit entries in submits hold TransferHandle; destruction uses op to release it
    GenericTransferOperator op;
    StubBackend backend;
    UsbInterface comm_intf{.interface_class = 0x02,
                           .interface_subclass = 0x02, // ACM (RNDIS wrapper)
                           .interface_protocol = 0xFF, // Vendor
                           .endpoints = {{UsbEndpoint{.address = EP_NOTIFY,
                                                       .attributes = 0x03,
                                                       .max_packet_size = 8,
                                                       .interval = 32}}}};
    UsbInterface data_intf{.interface_class = 0x0A,
                           .interface_subclass = 0x00,
                           .interface_protocol = 0x00,
                           .endpoints = {{UsbEndpoint{.address = EP_IN,
                                                       .attributes = 0x02,
                                                       .max_packet_size = 64,
                                                       .interval = 0},
                                          UsbEndpoint{.address = EP_OUT,
                                                       .attributes = 0x02,
                                                       .max_packet_size = 64,
                                                       .interval = 0}}}};
    RndisCommunicationInterfaceHandler comm;
    RndisDataInterfaceHandler data;
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    explicit RndisTestEnv(UsbSpeed speed = UsbSpeed::Full) :
        comm(comm_intf, pool, TEST_MAC, speed), data(data_intf, pool, &backend, &comm) {
        // Connection-failure scenarios outside this test's scope (normal on_new_connection path reports no error);
        // cannot ASSERT inside constructor (non-void return); error retained in ec member
        comm.on_new_connection(stub, ec);
        data.on_new_connection(stub, ec);
    }

    // Find RET_SUBMIT for seqnum (return nullptr if not found)
    const UsbIpResponse::UsbIpRetSubmit *find_submit(std::uint32_t seqnum) const {
        for (auto &s : stub.submits) {
            if (s.header.seqnum == seqnum) {
                return &s;
            }
        }
        return nullptr;
    }
};

// ========== Control-message construction (host→device, LE serialization) ==========

data_type make_init_msg(std::uint32_t request_id = 1) {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Init), 24u, request_id, 1u, 0u, 0u);
    return m;
}

data_type make_query_msg(std::uint32_t oid, std::uint32_t request_id = 1) {
    data_type m;
    // No input buffer: InformationBufferOffset=20 (relative to byte 8)
    vector_append_to_le(m, rndis_msg(RndisMessageType::Query), 28u, request_id, oid, 0u, 20u, 0u);
    return m;
}

data_type make_set_msg(std::uint32_t oid, const data_type &buf, std::uint32_t request_id = 1) {
    data_type m;
    // Note 28+buf.size() must explicitly cast to u32: vector_append_to_le serializes according to sizeof;
    // size_t would write 8 bytes, misaligning fields
    vector_append_to_le(m, rndis_msg(RndisMessageType::Set), static_cast<std::uint32_t>(28 + buf.size()), request_id,
                        oid, static_cast<std::uint32_t>(buf.size()), 20u, 0u);
    m.insert(m.end(), buf.begin(), buf.end());
    return m;
}

data_type make_keepalive_msg(std::uint32_t request_id = 1) {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Keepalive), 12u, request_id);
    return m;
}

data_type make_halt_msg() {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Halt), 12u, 1u);
    return m;
}

data_type make_reset_msg() {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Reset), 12u, 0u);
    return m;
}

// ========== Control-request submission ==========

// SEND_ENCAPSULATED_COMMAND (OUT class request, data phase = entire RNDIS message)
void send_command(RndisTestEnv &env, const data_type &msg, std::uint32_t seqnum = 1) {
    const SetupPacket setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x00,      // SEND_ENCAPSULATED_COMMAND
            .value = 0,
            .index = 0,
            .length = static_cast<std::uint16_t>(msg.size()),
    };
    env.comm.handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, msg.size(), setup,
            make_cmd_submit(env.op, seqnum, 0x00, UsbIpDirection::Out, msg.size(), setup, msg).transfer, env.ec);
    ASSERT_FALSE(env.ec);
}

// GET_ENCAPSULATED_RESPONSE (IN class request); return retrieved response bytes (empty on EPIPE)
data_type get_response(RndisTestEnv &env, std::uint32_t seqnum) {
    const SetupPacket setup{
            .request_type = 0xA1, // Class | Interface | IN
            .request = 0x01,      // GET_ENCAPSULATED_RESPONSE
            .value = 0,
            .index = 0,
            .length = 1558, // Host buffer (matching RNDIS_MAX_TOTAL_SIZE)
    };
    auto submit = make_cmd_submit(env.op, seqnum, 0x00, UsbIpDirection::In, 1558, setup);
    env.comm.handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, 1558, setup, std::move(submit.transfer), env.ec);
    EXPECT_FALSE(env.ec);
    if (env.ec) {
        return {};
    }
    const auto *ret = env.find_submit(seqnum);
    if (ret == nullptr) {
        return {};
    }
    return ret_submit_data(*ret);
}

// ========== Response-parsing helpers ==========

// Parse QUERY_C: return (status, payload)
std::pair<std::uint32_t, data_type> parse_query_cmplt(const data_type &resp) {
    EXPECT_GE(resp.size(), 24u);
    auto status = read_le32_at(resp, 12);
    auto payload_len = read_le32_at(resp, 16);
    auto payload_off = read_le32_at(resp, 20);
    EXPECT_EQ(payload_off, 16u); // Data immediately follows 24-byte header
    return {status, data_type(resp.begin() + 24, resp.begin() + 24 + payload_len)};
}

// ========== Packet construction (host direction: 24-byte header, DataOffset=16) ==========

// Single RNDIS_MSG_PACKET (host→device layout: 6 header fields, 24 bytes; data starts at byte 24)
data_type make_host_packet(const data_type &frame, std::uint32_t data_offset = 16, std::uint32_t oob_len = 0) {
    data_type p;
    // Same as above: explicitly cast 24+frame.size() to u32; size_t would write 8 bytes
    vector_append_to_le(p, 1u, static_cast<std::uint32_t>(24 + frame.size()), data_offset,
                        static_cast<std::uint32_t>(frame.size()), 0u, oob_len);
    p.insert(p.end(), frame.begin(), frame.end());
    return p;
}

} // namespace

// ==================== Control plane ====================

TEST(TestRndisHandler, SendEncapsulatedInitQueuesInitCmpltAndNotification) {
    // INIT → SEND_ENCAPSULATED_COMMAND returns OK; response enqueued and triggers
    // RESPONSE_AVAILABLE notification (interrupt IN endpoint, 8 bytes {1,0})
    RndisTestEnv env;

    // First leave interrupt IN request pending: notification should respond directly
    env.comm.handle_interrupt_transfer(9, UsbEndpoint{.address = EP_NOTIFY, .attributes = 0x03,
                                                      .max_packet_size = 8, .interval = 32},
                                       0, 8, make_cmd_submit(env.op, 9, 0x83, UsbIpDirection::In, 8).transfer, env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // No notification: request pending

    send_command(env, make_init_msg());
    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u); // Command received

    // Notification = 8 bytes {1, 0} (two LE32)
    const auto *notify = env.find_submit(9);
    ASSERT_NE(notify, nullptr);
    EXPECT_EQ(notify->status, 0u);
    EXPECT_EQ(ret_submit_data(*notify), (data_type{1, 0, 0, 0, 0, 0, 0, 0}));

    // Retrieve INIT_C: 52 bytes, fields match kernel rndis_init_cmplt
    auto resp = get_response(env, 2);
    ASSERT_EQ(resp.size(), 52u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::InitComplete));
    EXPECT_EQ(read_le32_at(resp, 4), 52u);
    EXPECT_EQ(read_le32_at(resp, 8), 1u); // RequestID echoed
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
    EXPECT_EQ(read_le32_at(resp, 16), 1u); // MajorVersion
    EXPECT_EQ(read_le32_at(resp, 20), 0u); // MinorVersion
    EXPECT_EQ(read_le32_at(resp, 24), RNDIS_DF_CONNECTIONLESS);
    EXPECT_EQ(read_le32_at(resp, 28), RNDIS_MEDIUM_802_3);
    EXPECT_EQ(read_le32_at(resp, 32), 1u); // MaxPacketsPerTransfer
    EXPECT_EQ(read_le32_at(resp, 36), RNDIS_MAX_TRANSFER_SIZE);
    EXPECT_EQ(read_le32_at(resp, 40), 0u); // PacketAlignmentFactor
}

TEST(TestRndisHandler, InitQueuesMediaConnectIndicate) {
    // After INIT completes, enqueue MEDIA_CONNECT status notification (matching kernel rndis_signal_connect:
    // Windows relies on this notification to confirm link up)
    RndisTestEnv env;
    send_command(env, make_init_msg());

    auto resp = get_response(env, 2); // INIT_C
    ASSERT_GE(resp.size(), 20u);
    resp = get_response(env, 3); // INDICATE MEDIA_CONNECT
    ASSERT_EQ(resp.size(), 20u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::Indicate));
    EXPECT_EQ(read_le32_at(resp, 4), 20u);
    EXPECT_EQ(read_le32_at(resp, 8), rndis_status(RndisStatus::MediaConnect));
    EXPECT_EQ(read_le32_at(resp, 12), 0u); // StatusBufferLength
    EXPECT_EQ(env.comm.get_state(), RndisState::Initialized);
}

TEST(TestRndisHandler, QueryOidReturnsData) {
    // QUERY OID_802_3_PERMANENT_ADDRESS → QUERY_C + 6-byte MAC
    // (Linux host binding requires querying this OID; failure prevents binding)
    RndisTestEnv env;
    send_command(env, make_query_msg(OID_802_3_PERMANENT_ADDRESS));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 24u + 6u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::QueryComplete));
    EXPECT_EQ(read_le32_at(resp, 8), 1u); // RequestID echoed
    auto [status, payload] = parse_query_cmplt(resp);
    EXPECT_EQ(status, rndis_status(RndisStatus::Success));
    EXPECT_EQ(payload, (data_type(TEST_MAC.begin(), TEST_MAC.end())));
}

TEST(TestRndisHandler, QueryUnsupportedOidReturnsNotSupported) {
    // Unknown OID → QUERY_C NOT_SUPPORTED (must not stall control request; matches kernel)
    RndisTestEnv env;
    send_command(env, make_query_msg(0x12345678));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 24u); // No data
    auto [status, payload] = parse_query_cmplt(resp);
    EXPECT_EQ(status, rndis_status(RndisStatus::NotSupported));
    EXPECT_TRUE(payload.empty());
}

TEST(TestRndisHandler, QueryLinkSpeedBySpeed) {
    // Report LINK_SPEED according to device speed (100bps units, matching kernel bitrate())
    RndisTestEnv env_full;
    send_command(env_full, make_query_msg(OID_GEN_LINK_SPEED));
    auto [status, payload] = parse_query_cmplt(get_response(env_full, 2));
    EXPECT_EQ(status, rndis_status(RndisStatus::Success));
    ASSERT_EQ(payload.size(), 4u);
    EXPECT_EQ(read_le32_at(payload, 0), 97'280u); // Full speed

    RndisTestEnv env_high(UsbSpeed::High);
    send_command(env_high, make_query_msg(OID_GEN_LINK_SPEED));
    auto [status2, payload2] = parse_query_cmplt(get_response(env_high, 2));
    ASSERT_EQ(payload2.size(), 4u);
    EXPECT_EQ(read_le32_at(payload2, 0), 4'259'840u); // High speed
}

TEST(TestRndisHandler, SetPacketFilterEnablesData) {
    // SET OID_GEN_CURRENT_PACKET_FILTER: nonzero → DATA_INITIALIZED (data gate open),
    // zero → INITIALIZED (closed). Matches rndis.c gen_ndis_set_resp
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2); // Retrieve INIT_C
    get_response(env, 3); // Retrieve MEDIA_CONNECT
    EXPECT_FALSE(env.comm.is_data_enabled());

    data_type filter_32{0x2D, 0x00, 0x00, 0x00}; // LE32 0x2D
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, filter_32));
    auto resp = get_response(env, 4);
    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::SetComplete));
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
    EXPECT_TRUE(env.comm.is_data_enabled());
    EXPECT_EQ(env.comm.get_state(), RndisState::DataInitialized);

    // filter=0: data gate closed
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type(4, 0)));
    get_response(env, 5);
    EXPECT_FALSE(env.comm.is_data_enabled());
    EXPECT_EQ(env.comm.get_state(), RndisState::Initialized);
}

TEST(TestRndisHandler, KeepaliveRespondsSuccess) {
    // KEEPALIVE → KEEPALIVE_C SUCCESS (Windows host sends keepalive every 5 seconds)
    RndisTestEnv env;
    send_command(env, make_keepalive_msg(7));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::KeepaliveComplete));
    EXPECT_EQ(read_le32_at(resp, 8), 7u); // RequestID echoed
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
}

TEST(TestRndisHandler, ResetClearsQueueAndResponds) {
    // RESET clears unretrieved response queue, returning RESET_C (AddressingReset=1). Matches
    // rndis.c: queued INIT_C/MEDIA_CONNECT must be discarded
    RndisTestEnv env;
    send_command(env, make_init_msg()); // Enqueue INIT_C + MEDIA_CONNECT
    send_command(env, make_reset_msg());

    auto resp = get_response(env, 2);
    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::ResetComplete));
    EXPECT_EQ(read_le32_at(resp, 8), rndis_status(RndisStatus::Success));
    EXPECT_EQ(read_le32_at(resp, 12), 1u); // AddressingReset

    // Queue contains only RESET_C; another retrieval should get EPIPE
    send_command(env, make_init_msg()); // Enqueue again
    auto resp2 = get_response(env, 3);
    ASSERT_EQ(resp2.size(), 52u); // New queue starts with INIT_C (RESET cleared old responses)
}

TEST(TestRndisHandler, HaltResetsStateNoResponse) {
    // HALT has no response (matching rndis.c); return to UNINITIALIZED, close data gate
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2);
    get_response(env, 3);
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type{0x2D, 0, 0, 0}));
    get_response(env, 4);
    EXPECT_TRUE(env.comm.is_data_enabled());

    send_command(env, make_halt_msg());
    // HALT has no response: retrieving response should get EPIPE
    const SetupPacket setup{
            .request_type = 0xA1, .request = 0x01, .value = 0, .index = 0, .length = 1558,
    };
    auto submit = make_cmd_submit(env.op, 5, 0x00, UsbIpDirection::In, 1558, setup);
    env.comm.handle_non_standard_request_type_control_urb(
            5, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, 1558, setup, std::move(submit.transfer), env.ec);
    ASSERT_FALSE(env.ec);
    const auto *ret = env.find_submit(5);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(env.comm.get_state(), RndisState::Uninitialized);
    EXPECT_FALSE(env.comm.is_data_enabled());
}

TEST(TestRndisHandler, GetResponseEmptyQueueStalls) {
    // GET_ENCAPSULATED_RESPONSE returns EPIPE when queue empty (matching kernel STALL:
    // host rndis_command polls/retries every 40ms)
    RndisTestEnv env;
    auto resp = get_response(env, 2);
    EXPECT_TRUE(resp.empty());
    const auto *ret = env.find_submit(2);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ==================== Data plane ====================

TEST(TestRndisHandler, OutPacketStrippedHeaderDeliveredToBackend) {
    // Host bulk OUT: RNDIS_MSG_PACKET (24B header, DataOffset=16) → after stripping header,
    // pass Ethernet frame to backend; respond actual_length=total transfer length
    RndisTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                             0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    auto payload = make_host_packet(frame);
    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, payload.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, payload.size(), {}, payload)
                                          .transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);

    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, payload.size());
}

TEST(TestRndisHandler, OutMultipacketAggregation) {
    // One bulk OUT transfer aggregates two RNDIS_MSG_PACKET messages (protocol permits it; host sends one per transfer,
    // but device must handle it) → backend receives two frames
    RndisTestEnv env;

    const data_type frame1 = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                              0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    // Frame length must be ≥ 14 (minimum Ethernet header length, matching kernel u_ether ETH_HLEN check)
    const data_type frame2 = {0xCC, 0xDD, 0x00, 0x02, 0x03, 0x04, 0x00, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    auto p1 = make_host_packet(frame1);
    auto p2 = make_host_packet(frame2);
    p1.insert(p1.end(), p2.begin(), p2.end());

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, p1.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, p1.size(), {}, p1).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 2u);
    EXPECT_EQ(env.backend.frames[0], frame1);
    EXPECT_EQ(env.backend.frames[1], frame2);
}

TEST(TestRndisHandler, OutToleratesOobAndPadding) {
    // Tolerate abnormal hosts: nonzero OOB fields, nonstandard DataOffset, message-tail padding—
    // locate frame using DataOffset/DataLength, ignore OOB directly (matching kernel rndis_rm_hdr)
    RndisTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x08, 0x00};
    // DataOffset=20 (data starts at byte 28; 4-byte padding after 24B header), declared OOB nonzero,
    // padding bytes appended to message tail
    auto payload = make_host_packet(frame, 20, 4);
    payload.insert(payload.begin() + 24, 4, 0xEE); // 4-byte padding between header and data
    payload.push_back(0xFF);                       // Tail padding (outside message, harmless)

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, payload.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, payload.size(), {}, payload)
                                          .transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);
}

TEST(TestRndisHandler, InFrameWrappedWithRndisHeader) {
    // send_frame → host bulk IN request retrieves it: 44-byte packet header
    // (MessageType=1/MessageLength=44+len/DataOffset=36/DataLength=len) + frame
    RndisTestEnv env;

    const data_type frame = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x08, 0x00};
    EXPECT_EQ(env.data.send_frame(frame), frame.size());

    env.data.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 1558, make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In, 1558).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    const auto *ret = env.find_submit(2);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 44u + frame.size());
    auto resp = ret_submit_data(*ret);
    ASSERT_EQ(resp.size(), 44u + frame.size());
    EXPECT_EQ(read_le32_at(resp, 0), 1u); // RNDIS_MSG_PACKET
    EXPECT_EQ(read_le32_at(resp, 4), 44u + frame.size());
    EXPECT_EQ(read_le32_at(resp, 8), 36u); // Data starts at byte 44
    EXPECT_EQ(read_le32_at(resp, 12), frame.size());
    EXPECT_EQ(read_le32_at(resp, 16), 0u); // OOB all zero (no checksum offloading)
    EXPECT_EQ(data_type(resp.begin() + 44, resp.end()), frame);
}

TEST(TestRndisHandler, InFramePadWhenMultipleOfMaxPacket) {
    // When packet total length is exactly a multiple of maxpacket(64), append 1 byte to prevent ZLP (matching kernel u_ether.c:
    // RNDIS disallows ZLP; MessageLength excludes padding byte; after host splits packets by message length,
    // tail byte is trimmed). 44+20=64 → should send 65 bytes
    RndisTestEnv env;

    const data_type frame(20, 0xAB);
    EXPECT_EQ(env.data.send_frame(frame), frame.size());

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 1558, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 1558).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->actual_length, 65u); // 44 + 20 + 1 padding
    auto resp = ret_submit_data(*ret);
    ASSERT_EQ(resp.size(), 65u);
    EXPECT_EQ(read_le32_at(resp, 4), 64u); // MessageLength excludes padding
    EXPECT_EQ(resp.back(), 0u);
}

TEST(TestRndisHandler, UnlinkPendingInRequest) {
    // UNLINK cancels pending IN request: return RET_UNLINK(-ECONNRESET)
    RndisTestEnv env;

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // No frame: request pending

    env.data.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(env.stub.submits.empty());
}

TEST(TestRndisHandler, DisconnectResetsStateAndChannels) {
    // Disconnect: state machine reset (UNINITIALIZED, response queue cleared, data gate closed),
    // reconnection starts clean
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2);
    get_response(env, 3);
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type{0x2D, 0, 0, 0}));
    get_response(env, 4);
    EXPECT_TRUE(env.comm.is_data_enabled());

    std::error_code disc_ec;
    env.comm.on_disconnection(disc_ec);
    env.data.on_disconnection(disc_ec);
    EXPECT_EQ(env.comm.get_state(), RndisState::Uninitialized);
    EXPECT_FALSE(env.comm.is_data_enabled());

    // Reconnect: queue clean, no leftover responses (INIT starts over)
    std::error_code conn_ec;
    env.comm.on_new_connection(env.stub, conn_ec);
    env.data.on_new_connection(env.stub, conn_ec);
    send_command(env, make_init_msg());
    auto resp = get_response(env, 5);
    ASSERT_EQ(resp.size(), 52u); // First item is INIT_C, no leftovers
}
