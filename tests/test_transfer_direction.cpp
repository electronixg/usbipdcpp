// RET_SUBMIT / CMD_SUBMIT transmitted byte-stream behavior tests: direction (IN/OUT) × transfer type
// (bulk/iso) × actual_length combinations; verify whether protocol-layer length calculated with transfer_is_in
// is correct—core regression: iso OUT response sends only iso descriptors, never received data as return payload
// (before fix, GenericTransferOperator sent data when length>0, misread by vhci as descriptors)

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

namespace {

/// Local TCP loopback socket pair: to_socket writes send_sock; recv_sock reader verifies
struct SocketPair {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor;
    asio::ip::tcp::socket send_sock;
    asio::ip::tcp::socket recv_sock;

    SocketPair()
        : acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
          send_sock(io),
          recv_sock(io) {
        auto ep = acceptor.local_endpoint();
        acceptor.listen(1);
        std::thread accept_thread([this] { acceptor.accept(recv_sock); });
        send_sock.connect(ep);
        accept_thread.join();
    }
};

std::vector<std::uint8_t> read_n(asio::ip::tcp::socket &sock, std::size_t n) {
    std::vector<std::uint8_t> buf(n);
    asio::read(sock, asio::buffer(buf));
    return buf;
}

std::uint32_t be32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

/// Construct RET simulating real response path: alloc_transfer_handle (records direction) → fill data →
/// create_ret_submit. payload is data-phase content; iso_actual gives actual bytes per packet
UsbIpResponse::UsbIpRetSubmit make_ret(GenericTransferOperator &op, bool is_in, std::uint32_t seqnum,
                                       std::uint32_t actual_length, int num_iso,
                                       const std::vector<std::uint8_t> &payload,
                                       const std::vector<std::uint32_t> &iso_actual = {}) {
    UsbIpHeaderBasic header{};
    header.direction = is_in ? UsbIpDirection::In : UsbIpDirection::Out;
    auto *raw = op.alloc_transfer_handle(payload.size(), num_iso, header, {});
    auto *trx = GenericTransfer::from_handle(raw);
    trx->data = payload;
    trx->actual_length = payload.size();
    std::uint32_t offset = 0;
    for (int i = 0; i < num_iso; ++i) {
        auto actual = i < static_cast<int>(iso_actual.size()) ? iso_actual[i] : 0;
        trx->iso_descriptors[i] = {.offset = offset, .length = 96, .actual_length = actual, .status = 0};
        offset += 96;
    }
    TransferHandle handle(raw, &op);
    return UsbIpResponse::UsbIpRetSubmit::create_ret_submit(seqnum, 0, actual_length, 0, num_iso,
                                                            std::move(handle));
}

/// Verify 48-byte usbip_header (big-endian layout in protocol documentation: base 20 bytes + ret_submit 20 bytes + padding 8)
void expect_header(const std::vector<std::uint8_t> &hdr, std::uint32_t seqnum, std::uint32_t actual_length,
                   std::uint32_t num_packets) {
    ASSERT_EQ(hdr.size(), 48u);
    EXPECT_EQ(be32(hdr, 0), USBIP_RET_SUBMIT);
    EXPECT_EQ(be32(hdr, 4), seqnum);
    EXPECT_EQ(be32(hdr, 20), 0u); // status
    EXPECT_EQ(be32(hdr, 24), actual_length);
    EXPECT_EQ(be32(hdr, 32), num_packets);
}

} // namespace

// ============== Allocation direction recording ==============

TEST(TestTransferDirection, AllocRecordsDirection) {
    GenericTransferOperator op;
    UsbIpHeaderBasic header{};

    header.direction = UsbIpDirection::In;
    auto *in_handle = op.alloc_transfer_handle(64, 0, header, {});
    EXPECT_TRUE(op.transfer_is_in(in_handle));
    op.free_transfer_handle(in_handle);

    header.direction = UsbIpDirection::Out;
    auto *out_handle = op.alloc_transfer_handle(64, 0, header, {});
    EXPECT_FALSE(op.transfer_is_in(out_handle));
    op.free_transfer_handle(out_handle);
}

// ============== RET_SUBMIT byte stream ==============

TEST(TestTransferDirection, RetSubmitNonIsoInSendsData) {
    GenericTransferOperator op;
    SocketPair sp;
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    auto ret = make_ret(op, /*is_in=*/true, 100, 5, 0, payload);
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 100, 5, 0);
    // IN: data returned, content matches payload
    auto data = read_n(sp.recv_sock, 5);
    EXPECT_EQ(data, payload);
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitNonIsoOutWithoutTransferSendsHeaderOnly) {
    // Existing OUT devices' approach: response without transfer (create_ret_submit_ok_without_data),
    // send only header—actual_length truthfully reports received byte count; data not returned
    SocketPair sp;
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(100, 64);
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 100, 64, 0);
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitIsoInSendsDataAndDescriptors) {
    GenericTransferOperator op;
    SocketPair sp;
    // 2 iso packets, each with a 96-byte slot; actual reception 96 + 48
    const std::vector<std::uint8_t> payload(144, 0x7A);
    auto ret = make_ret(op, /*is_in=*/true, 101, 144, 2, payload, {96, 48});
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 101, 144, 2);
    // IN: data (compact, actual bytes per packet) precedes descriptors
    auto data = read_n(sp.recv_sock, 144);
    EXPECT_EQ(data, payload);
    auto desc = read_n(sp.recv_sock, 2 * 16);
    EXPECT_EQ(be32(desc, 0), 0u);   // Packet 0 offset
    EXPECT_EQ(be32(desc, 4), 96u);  // Packet 0 length
    EXPECT_EQ(be32(desc, 8), 96u);  // Packet 0 actual_length
    EXPECT_EQ(be32(desc, 16), 96u); // Packet 1 offset
    EXPECT_EQ(be32(desc, 20), 96u); // Packet 1 length
    EXPECT_EQ(be32(desc, 24), 48u); // Packet 1 actual_length
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitIsoOutSendsDescriptorsOnly) {
    // Core regression: iso OUT response (speaker scenario). actual_length truthfully set to 576 (vhci
    // checks sum(iso actual) == actual_length), but data must not be returned—before fix,
    // passing actual_length to to_socket caused vhci to misread 576 data bytes as descriptors
    GenericTransferOperator op;
    SocketPair sp;
    const std::vector<std::uint8_t> payload(576, 0x5A); // PCM sent by host (should be discarded, not returned)
    auto ret = make_ret(op, /*is_in=*/false, 102, 576, 6, payload, {96, 96, 96, 96, 96, 96});
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 102, 576, 6);
    // OUT: no data phase, only 6 descriptors
    auto desc = read_n(sp.recv_sock, 6 * 16);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16), static_cast<std::uint32_t>(i * 96));
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16 + 4), 96u);
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16 + 8), 96u);
    }
    // Key assertion: no extra bytes (before fix, this read 576 PCM bytes)
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

// ============== CMD_SUBMIT data-phase direction ==============

TEST(TestTransferDirection, CmdSubmitOutSendsDataInSendsNone) {
    // CMD_SUBMIT carries data phase only for OUT (symmetric with from_socket); IN always passes 0,
    // preventing accidental transmission of IN buffer contents
    GenericTransferOperator op;

    // OUT: send transfer_buffer_length bytes
    {
        SocketPair sp;
        UsbIpHeaderBasic header{};
        header.direction = UsbIpDirection::Out;
        auto *raw = op.alloc_transfer_handle(4, 0, header, {});
        auto *trx = GenericTransfer::from_handle(raw);
        trx->data = {9, 8, 7, 6};
        TransferHandle handle(raw, &op);
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 200;
        submit.header.direction = UsbIpDirection::Out;
        submit.transfer_buffer_length = 4;
        submit.transfer = std::move(handle);
        usbipdcpp::error_code ec;
        submit.to_socket(sp.send_sock, ec);
        ASSERT_FALSE(ec);
        auto hdr = read_n(sp.recv_sock, 48);
        EXPECT_EQ(be32(hdr, 0), USBIP_CMD_SUBMIT);
        auto data = read_n(sp.recv_sock, 4);
        EXPECT_EQ(data, (std::vector<std::uint8_t>{9, 8, 7, 6}));
    }

    // IN (violating "IN carries no transfer" convention): still must not send data (transfer_is_in safeguard)
    {
        SocketPair sp;
        UsbIpHeaderBasic header{};
        header.direction = UsbIpDirection::In;
        auto *raw = op.alloc_transfer_handle(4, 0, header, {});
        auto *trx = GenericTransfer::from_handle(raw);
        trx->data = {1, 1, 1, 1}; // Buffered content must not be sent
        TransferHandle handle(raw, &op);
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 201;
        submit.header.direction = UsbIpDirection::In;
        submit.transfer_buffer_length = 4;
        submit.transfer = std::move(handle);
        usbipdcpp::error_code ec;
        submit.to_socket(sp.send_sock, ec);
        ASSERT_FALSE(ec);
        auto hdr = read_n(sp.recv_sock, 48);
        EXPECT_EQ(be32(hdr, 0), USBIP_CMD_SUBMIT);
        EXPECT_EQ(sp.recv_sock.available(), 0u);
    }
}
