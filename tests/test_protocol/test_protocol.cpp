#include <gtest/gtest.h>

#include "usbipdcpp/protocol.h"
#include "test_utils.h"
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipdcpp;
using namespace usbipdcpp::test;

// Mock DeviceHandler for testing
class MockDeviceHandlerForTest : public AbstDeviceHandler {
public:
    explicit MockDeviceHandlerForTest(UsbDevice &device) : AbstDeviceHandler(device) {}

    void on_new_connection(TransferResponder &current_session, error_code &ec) override {}
    void on_disconnection(error_code &ec) override {}

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {}

    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd,
                     UsbEndpoint ep,
                     std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override {}
};

TEST(TestProtocol, UsbIpHeaderBasic) {
    UsbIpHeaderBasic header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x80
    };

    auto as_byte = header.to_bytes();
    data_type target_data = {
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x12, 0x34,
            0x00, 0x00, 0x56, 0x78,
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x80
    };
    ASSERT_EQ(as_byte.size(), target_data.size());
    for (std::size_t i = 0; i < as_byte.size(); i++) {
        ASSERT_EQ(as_byte[i], target_data[i]);
    }
}

TEST(TestProtocol, UsbIpHeaderBasicReadSocket) {
    UsbIpHeaderBasic origin_header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x80
    };
    auto received_header = reread_from_socket_with_command<UsbIpHeaderBasic>(origin_header, USBIP_CMD_SUBMIT);
    expect_header_equal(received_header, origin_header);
}

TEST(TestProtocol, UsbIpCmdSubmitReadSocketWithoutData) {
    // IN transfer, no data to read.
    // Note ep has no direction bit (protocol semantics; see protocol.h UsbIpHeaderBasic::ep
    // comment); direction comes from direction field; cannot write 0x80 (rejected by parsing validation)
    UsbIpHeaderBasic header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x08
    };

    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(io_context);
    auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
    acceptor.open(server_endpoint.protocol());
    acceptor.bind(server_endpoint);
    acceptor.listen();
    auto server_port = acceptor.local_endpoint().port();

    std::thread sender([&]() {
        auto sock = acceptor.accept();
        usbipdcpp::data_type buffer;
        // Send version number + command code
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_VERSION));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_CMD_SUBMIT));

        // from_socket expects data beginning at seqnum (excluding command)
        // Thus send only seqnum, devid, direction, ep (16 bytes)
        usbipdcpp::vector_append_to_net(buffer, header.seqnum);
        usbipdcpp::vector_append_to_net(buffer, header.devid);
        usbipdcpp::vector_append_to_net(buffer, header.direction);
        usbipdcpp::vector_append_to_net(buffer, header.ep);

        // transfer_flags = 0x1234
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x1234));
        // transfer_buffer_length = 0
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        // start_frame = 0x8765
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x8765));
        // number_of_packets = 0
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        // interval = 0x1111
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x1111));
        // setup packet (8 bytes)
        buffer.insert(buffer.end(), {1, 2, 3, 4, 5, 6, 7, 8});

        sock.send(asio::buffer(buffer));
    });

    // Create test device (declare first because mock_handler references it)
    auto test_device = UsbDevice{
        .path = "/test",
        .busid = "1-1",
        .bus_num = 1,
        .dev_num = 1,
        .speed = 0,
        .vendor_id = 0,
        .product_id = 0,
        .device_bcd = 0,
        .device_class = 0,
        .device_subclass = 0,
        .device_protocol = 0,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = {},
        .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
        .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full)
    };
    MockDeviceHandlerForTest mock_handler(test_device);

    {
        // Use scope to ensure received destroyed before mock_handler
        UsbIpCommand::UsbIpCmdSubmit received{};
        asio::ip::tcp::socket server_socket(io_context);
        asio::error_code ec;
        server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket);
        [[maybe_unused]] auto op_command = usbipdcpp::read_u16(server_socket);

        received.transfer.set_operator(mock_handler.get_transfer_operator());
        received.from_socket(server_socket);
        received.header.command = op_command;

        server_socket.close();
        sender.join();

        EXPECT_EQ(received.header.seqnum, header.seqnum);
        EXPECT_EQ(received.transfer_flags, 0x1234);
        EXPECT_EQ(received.start_frame, 0x8765);
        EXPECT_EQ(received.interval, 0x1111);
    }  // received destroyed here while mock_handler still valid

    // mock_handler and test_device destroyed here
}

namespace {
// Simulate client sending IN-direction CMD_SUBMIT (no data phase),
// receive using get_cmd_from_socket. Caller must own handler,
// which must outlive transfer in out (transfer destructor needs handler's operator).
void send_cmd_submit_with_packets(std::uint32_t number_of_packets, AbstDeviceHandler &handler,
                                  UsbIpCommand::CmdVariant &out, usbipdcpp::error_code &out_ec) {
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(io_context);
    auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
    acceptor.open(server_endpoint.protocol());
    acceptor.bind(server_endpoint);
    acceptor.listen();
    auto server_port = acceptor.local_endpoint().port();

    std::thread sender([&]() {
        auto sock = acceptor.accept();
        usbipdcpp::data_type buffer;
        // get_cmd_from_socket first reads 4-byte command
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(USBIP_CMD_SUBMIT));
        // header fields (excluding command): seqnum, devid, direction, ep
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x1234));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(UsbIpDirection::In));
        // ep has no direction bit (protocol semantics; see protocol.h UsbIpHeaderBasic::ep comment);
        // direction comes from direction field; cannot write 0x80 (rejected by parsing validation)
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x08));
        // transfer parameters: transfer_flags, transfer_buffer_length, start_frame, number_of_packets, interval
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(64));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        usbipdcpp::vector_append_to_net(buffer, number_of_packets);
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        // setup packet (8 bytes)
        buffer.insert(buffer.end(), {0, 0, 0, 0, 0, 0, 0, 0});

        sock.send(asio::buffer(buffer));
    });

    asio::ip::tcp::socket server_socket(io_context);
    asio::error_code connect_ec;
    server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), connect_ec);
    if (connect_ec) {
        sender.join();
        out_ec = connect_ec;
        return;
    }

    out = UsbIpCommand::get_cmd_from_socket(server_socket, &handler, out_ec);
    server_socket.close();
    sender.join();
}
} // namespace

TEST(TestProtocol, UsbIpCmdSubmitRejectsTooManyIsoPackets) {
    // Malicious client sends excessive number_of_packets: return error and disconnect this session rather than crashing with exception
    auto test_device = UsbDevice{
        .path = "/test",
        .busid = "1-1",
        .bus_num = 1,
        .dev_num = 1,
        .speed = 0,
        .vendor_id = 0,
        .product_id = 0,
        .device_bcd = 0,
        .device_class = 0,
        .device_subclass = 0,
        .device_protocol = 0,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = {},
        .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
        .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full)
    };
    MockDeviceHandlerForTest mock_handler(test_device);
    UsbIpCommand::CmdVariant ret;
    usbipdcpp::error_code ec;
    send_cmd_submit_with_packets(USBIPDCPP_MAX_ISO_PACKETS + 1, mock_handler, ret, ec);

    EXPECT_TRUE(ec);
    EXPECT_EQ(ec.value(), static_cast<int>(ErrorType::SOCKET_ERR));
    // ret destroyed before mock_handler; operator still valid when transfer released
}

TEST(TestProtocol, UsbIpCmdSubmitAcceptsAllOnesPacketsPlaceholder) {
    // 0xFFFFFFFF is protocol placeholder for non-isochronous transfers; must allow, not misidentify as exceeding limit
    auto test_device = UsbDevice{
        .path = "/test",
        .busid = "1-1",
        .bus_num = 1,
        .dev_num = 1,
        .speed = 0,
        .vendor_id = 0,
        .product_id = 0,
        .device_bcd = 0,
        .device_class = 0,
        .device_subclass = 0,
        .device_protocol = 0,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = {},
        .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
        .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full)
    };
    MockDeviceHandlerForTest mock_handler(test_device);
    UsbIpCommand::CmdVariant ret;
    usbipdcpp::error_code ec;
    send_cmd_submit_with_packets(0xFFFFFFFF, mock_handler, ret, ec);

    ASSERT_FALSE(ec);
    ASSERT_TRUE(std::holds_alternative<UsbIpCommand::UsbIpCmdSubmit>(ret));
    EXPECT_EQ(std::get<UsbIpCommand::UsbIpCmdSubmit>(ret).number_of_packets, 0xFFFFFFFFu);
    // ret destroyed before mock_handler; operator still valid when transfer released
}

TEST(TestProtocol, UsbIpCmdUnlinkReadSocket) {

    UsbIpCommand::UsbIpCmdUnlink origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x80
            },
            .unlink_seqnum = 0xabcd
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdUnlink>(origin, USBIP_CMD_UNLINK);

    expect_cmd_unlink_equal(received, origin);
}

// ============== Edge-case tests ==============

TEST(TestProtocol, UsbIpHeaderBasicAllZeros) {
    UsbIpHeaderBasic header{
            .command = 0,
            .seqnum = 0,
            .devid = 0,
            .direction = 0,
            .ep = 0
    };

    auto bytes = header.to_bytes();
    for (auto byte : bytes) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(TestProtocol, UsbIpHeaderBasicMaxValues) {
    UsbIpHeaderBasic header{
            .command = 0xFFFFFFFF,
            .seqnum = 0xFFFFFFFF,
            .devid = 0xFFFFFFFF,
            .direction = 0xFFFFFFFF,
            .ep = 0xFFFFFFFF
    };

    auto bytes = header.to_bytes();
    EXPECT_EQ(bytes.size(), 20);

    // Verify byte order (big-endian)
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0xFF);
    EXPECT_EQ(bytes[3], 0xFF);
}

TEST(TestProtocol, UsbIpHeaderBasicRoundTrip) {
    UsbIpHeaderBasic original{
            .command = USBIP_RET_SUBMIT,
            .seqnum = 0xDEADBEEF,
            .devid = 0x12345678,
            .direction = UsbIpDirection::In,
            .ep = 0x0F
    };

    auto received = reread_from_socket_with_command<UsbIpHeaderBasic>(original, USBIP_RET_SUBMIT);
    expect_header_equal(received, original);
}

TEST(TestProtocol, UsbIpIsoPacketDescriptorRoundTrip) {
    UsbIpIsoPacketDescriptor original{
            .offset = 0x12345678,
            .length = 0xDEADBEEF,
            .actual_length = 0xFFFFFFFF,
            .status = 0
    };

    auto bytes = original.to_bytes();
    EXPECT_EQ(bytes.size(), 16);

    // Big-endian verification
    EXPECT_EQ(bytes[0], 0x12);
    EXPECT_EQ(bytes[1], 0x34);
    EXPECT_EQ(bytes[2], 0x56);
    EXPECT_EQ(bytes[3], 0x78);
}

TEST(TestProtocol, UsbIpIsoPacketDescriptorAllZeros) {
    UsbIpIsoPacketDescriptor desc{
            .offset = 0,
            .length = 0,
            .actual_length = 0,
            .status = 0
    };

    auto bytes = desc.to_bytes();
    for (auto byte : bytes) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(TestProtocol, UsbIpCmdUnlinkMaxSeqnum) {
    UsbIpCommand::UsbIpCmdUnlink origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_UNLINK,
                    .seqnum = 0xFFFFFFFF,
                    .devid = 0,
                    .direction = 0,
                    .ep = 0
            },
            .unlink_seqnum = 0xFFFFFFFF
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdUnlink>(origin, USBIP_CMD_UNLINK);
    expect_cmd_unlink_equal(received, origin);
}

TEST(TestProtocol, OpReqDevlistRoundTrip) {
    UsbIpCommand::OpReqDevlist origin{.status = 0};
    auto bytes = origin.to_bytes();

    // Verify version(2) + command(2) + status(4) = 8 bytes
    EXPECT_EQ(bytes.size(), 8);
}

TEST(TestProtocol, OpReqImportRoundTrip) {
    UsbIpCommand::OpReqImport origin{
            .status = 0,
            .busid = {'1', '-', '1', '5', '\0'}
    };

    // Fill remaining bytes with 0
    for (std::size_t i = 5; i < 32; ++i) {
        origin.busid[i] = 0;
    }

    auto bytes = origin.to_bytes();
    EXPECT_GT(bytes.size(), 0);
}

TEST(TestProtocol, DifferentEndpointAddresses) {
    // Test different endpoint addresses
    for (std::uint8_t ep = 0; ep <= 0x0F; ++ep) {
        UsbIpHeaderBasic header{
                .command = USBIP_CMD_SUBMIT,
                .seqnum = 0x1234,
                .devid = 0,
                .direction = UsbIpDirection::In,
                .ep = static_cast<std::uint32_t>(ep | 0x80)
        };

        auto bytes = header.to_bytes();
        EXPECT_EQ(bytes[19], static_cast<std::uint8_t>(ep | 0x80));
    }
}

TEST(TestProtocol, AllCommandTypes) {
    // Test all command types
    std::vector<std::uint32_t> commands = {
            USBIP_CMD_SUBMIT,
            USBIP_CMD_UNLINK,
            USBIP_RET_SUBMIT,
            USBIP_RET_UNLINK
    };

    for (auto cmd : commands) {
        UsbIpHeaderBasic header{
                .command = cmd,
                .seqnum = 0x1234,
                .devid = 0,
                .direction = 0,
                .ep = 0
        };

        auto bytes = header.to_bytes();
        EXPECT_EQ(bytes[3], static_cast<std::uint8_t>(cmd & 0xFF));
    }
}
