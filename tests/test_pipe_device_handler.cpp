#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// Pipe device + handler pointer (handler owned by device, same lifetime as device)
struct PipeFixture {
    std::shared_ptr<UsbDevice> device;
    PipeDeviceHandler *pipe = nullptr;
    // Transfer allocator: fixture member (fixture declared before test-local stub and destroyed afterward)—
    // RetSubmit entries in submits hold TransferHandle; destruction uses op to release it
    GenericTransferOperator op;
};

// Construct pipe device (same construction as examples/mock_pipe):
// vendor-class interface + bulk IN(0x81) + bulk OUT(0x02)
PipeFixture make_pipe_device(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF, // vendor specific
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_pipe",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5690,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();
    return {device, pipe.get()};
}

// Construct pipe device with two OUT endpoints (bulk IN 0x81 + bulk OUT 0x02/0x04),
// to verify global ordering of read across endpoints
PipeFixture make_pipe_device_two_out(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF,
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x04, // OUT
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_pipe_two_out",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5690,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();
    return {device, pipe.get()};
}

} // namespace

TEST(TestPipeDeviceHandler, OutDataReachesRead) {
    // OUT transfer: read() returns data; ep matches data; handler returns OK RET_SUBMIT
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const data_type payload = {'p', 'i', 'p', 'e', '!'};

    // Application thread blocks in read; main thread submits OUT transfer
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x02, UsbIpDirection::Out, payload.size(), {}, payload),
                              intf.endpoints[0][1], intf, ec);
    ASSERT_FALSE(ec);

    reader.join();
    EXPECT_EQ(xfer.ep, 0x02);
    EXPECT_FALSE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.data, payload);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, payload.size());

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ReadPreservesGlobalRequestOrderAcrossEndpoints) {
    // Across endpoints, return in global host-request arrival order (seqnum order), rather than endpoint-number order:
    // send ep2(seq 1) → ep4(seq 2) → ep2(seq 3); read must retrieve 1, 2, 3 in order
    StringPool string_pool;
    auto fixture = make_pipe_device_two_out(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_out2 = intf.endpoints[0][1];
    const UsbEndpoint ep_out4 = intf.endpoints[0][2];

    // Application thread blocks reading 3 items
    std::vector<PipeXfer> got;
    std::thread reader([&]() {
        for (int i = 0; i < 3; i++) {
            PipeXfer xfer;
            if (fixture.pipe->read(xfer, 2000)) {
                got.push_back(std::move(xfer));
            }
        }
    });

    // Interleaved sends: ep 0x02 → ep 0x04 → ep 0x02
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x02, UsbIpDirection::Out, 1, {}, {0x01}), ep_out2, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 2, 0x04, UsbIpDirection::Out, 1, {}, {0x02}), ep_out4, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 3, 0x02, UsbIpDirection::Out, 1, {}, {0x03}), ep_out2, intf, ec);
    ASSERT_FALSE(ec);

    reader.join();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].ep, 0x02);
    EXPECT_EQ(got[0].data, (data_type{0x01}));
    EXPECT_EQ(got[1].ep, 0x04);
    EXPECT_EQ(got[1].data, (data_type{0x02}));
    EXPECT_EQ(got[2].ep, 0x02);
    EXPECT_EQ(got[2].data, (data_type{0x03}));

    ASSERT_EQ(stub.submits.size(), 3u);
    for (std::size_t i = 0; i < 3; i++) {
        EXPECT_EQ(stub.submits[i].header.seqnum, static_cast<std::uint32_t>(i + 1));
        EXPECT_EQ(stub.submits[i].status, 0u);
        EXPECT_EQ(stub.submits[i].actual_length, 1u);
    }

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, InWriteServesPendingRequests) {
    // IN requests initially pending (FIFO has no data); after write, respond in request-length fragments:
    // two 8-byte requests each retrieve half of the 16-byte data
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // First leave two IN requests pending (each expects 8 bytes)
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 2, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);

    // Write 16 bytes in one operation; each pending request retrieves 8 bytes
    const data_type payload = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 2000), payload.size());

    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 8u);
    EXPECT_EQ(ret_submit_data(stub.submits[0]), data_type(payload.begin(), payload.begin() + 8));

    EXPECT_EQ(stub.submits[1].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(stub.submits[1].actual_length, 8u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type(payload.begin() + 8, payload.end()));

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ControlRequestDeliveredToRead) {
    // Nonstandard control request (Vendor+Interface+IN): after read() obtains setup_req,
    // write({ep=0}) responds; handler returns RET_SUBMIT with data
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const SetupPacket setup{
            .request_type = 0xC1, // Vendor(0x40) | Interface(0x01) | IN(0x80)
            .request = 0x11,
            .value = 0x1234,
            .index = 0,
            .length = 4,
    };

    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x00, UsbIpDirection::In, 4, setup), fixture.device->ep0_in,
                              std::nullopt, ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.ep, 0);
    EXPECT_EQ(xfer.setup_req->request_type, 0xC1u);
    EXPECT_EQ(xfer.setup_req->request, 0x11u);
    EXPECT_EQ(xfer.setup_req->value, 0x1234u);
    EXPECT_EQ(xfer.setup_req->length, 4u);
    EXPECT_TRUE(xfer.data.empty());

    // write({ep=0}) responds to control request
    const data_type reply = {'o', 'k', 'y', '!'};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0, .data = reply}, 2000), reply.size());

    ASSERT_EQ(stub.submits.size(), 1u); // Retrieving control IN notification alone does not respond; only write produces response
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, reply.size());
    EXPECT_EQ(ret_submit_data(stub.submits[0]), reply);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, DisconnectUnblocksReadWrite) {
    // Disconnection (on_disconnection) awakens blocked read (returns false) and write (returns 0)
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // read waits indefinitely; handler cleans up and awakens on disconnection
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_FALSE(fixture.pipe->read(xfer, 0)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Ensure blocked
    fixture.pipe->on_disconnection(ec);
    reader.join();

    // After disconnection, write immediately returns 0 (no longer awaiting FIFO space)
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = {1, 2, 3}}, 0), 0u);
}

TEST(TestPipeDeviceHandler, FifoFullBlocksWriteUntilHostConsumes) {
    // write blocks when FIFO full (matching kernel FIFO semantics); after host IN request retrieves data,
    // continue writing remainder; bytes retrieved in two operations concatenate to original data
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(16); // Set before connection; FIFO holds only 16 bytes
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    const data_type payload = [] {
        data_type data;
        for (std::uint8_t i = 0; i < 32; i++) {
            data.push_back(i);
        }
        return data;
    }();

    std::size_t written = 0;
    std::thread writer([&]() { written = fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Ensure write blocked on full FIFO

    // Host retrieves first 16 bytes: write finishes remainder after space freed
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x01, UsbIpDirection::In, 16), ep_in, intf, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].actual_length, 16u);
    EXPECT_EQ(ret_submit_data(stub.submits[0]), data_type(payload.begin(), payload.begin() + 16));

    writer.join();
    EXPECT_EQ(written, payload.size());

    // Retrieve remaining 16 bytes
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 2, 0x01, UsbIpDirection::In, 16), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[1].actual_length, 16u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type(payload.begin() + 16, payload.end()));

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, CustomDescriptorServedToHost) {
    // Report descriptor set by set_custom_descriptor returned through GET_DESCRIPTOR(0x22)
    // (standard request recipient=interface): HID devices use this API to provide report descriptors
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    // Simplified keyboard report descriptor (verifying mechanism; content unrestricted)
    const data_type report_desc = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0};
    fixture.pipe->set_custom_descriptor(0x22, report_desc);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // GET_DESCRIPTOR(Report): IN | Standard | Interface, wValue high byte=0x22
    const SetupPacket setup{
            .request_type = 0x81,
            .request = 0x06, // GET_DESCRIPTOR
            .value = 0x2200,
            .index = 0,
            .length = static_cast<std::uint16_t>(report_desc.size()),
    };
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x00, UsbIpDirection::In, report_desc.size(), setup),
                              fixture.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, report_desc.size());
    EXPECT_EQ(ret_submit_data(stub.submits[0]), report_desc);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ControlOutRequestDeliveredToRead) {
    // OUT-direction control request: read() obtains both setup_req and data phase
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // Vendor+Interface+OUT control request, carrying 3 data bytes (e.g. HID SET_REPORT)
    const SetupPacket setup{
            .request_type = 0x41, // Vendor(0x40) | Interface(0x01) | OUT(0x00)
            .request = 0x09,
            .value = 0x0200,
            .index = 0,
            .length = 3,
    };
    const data_type payload = {'L', 'E', 'D'};

    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x00, UsbIpDirection::Out, payload.size(), setup, payload),
                              fixture.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.ep, 0);
    EXPECT_EQ(xfer.setup_req->request_type, 0x41u);
    EXPECT_EQ(xfer.setup_req->length, 3u);
    EXPECT_EQ(xfer.data, payload);

    // handler already returned OK (OUT has no data phase)
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, payload.size());

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ReadWriteTimeout) {
    // read/write timeout semantics: without data/space, return according to timeout_ms (no indefinite blocking)
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(8); // Small FIFO lets write fill it quickly
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // read timeout: returns false after 100ms without data
    PipeXfer xfer;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(fixture.pipe->read(xfer, 100));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));

    // write timeout: with FIFO full and no IN requests, writes only FIFO-capacity portion
    const data_type payload(16, 0xAB);
    auto written = fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 100);
    EXPECT_EQ(written, 8u); // Capacity 8; stop once full

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, UnlinkPendingRequest) {
    // UNLINK cancels pending IN request: return RET_UNLINK with status -ECONNRESET
    // (matching kernel stub_tx.c handling of canceled URBs)
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // IN request pending (FIFO has no data)
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    // Cancel that request
    fixture.pipe->handle_unlink_seqnum(1, 2);

    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u); // RET_UNLINK returns the unlink command's own seqnum
    EXPECT_EQ(stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(stub.submits.empty()); // Canceled request sends no further RET_SUBMIT

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, UnlinkCompletedRequestReportsSuccess) {
    // UNLINK target already completed normally (RET_SUBMIT sent): request no longer in pending queue,
    // RET_UNLINK returns 0 (URB complete; matching stub_tx.c: target not found returns 0)
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // IN request pending, then write responds (transfer completed)
    fixture.pipe->receive_urb(make_cmd_submit(fixture.op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    const data_type payload = {1, 2, 3, 4};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 2000), payload.size());
    ASSERT_EQ(stub.submits.size(), 1u); // RET_SUBMIT sent

    // Cancel completed target: return 0
    fixture.pipe->handle_unlink_seqnum(1, 2);
    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(stub.unlinks[0].status, 0u);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, TriggerSessionStopNotifiesResponder) {
    // On device unplug/error, trigger_session_stop → responder->stop_transfer()
    // (notify Session to stop immediately; interface-decoupled stop chain)
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);
    EXPECT_FALSE(stub.stopped);

    fixture.pipe->trigger_session_stop();
    EXPECT_TRUE(stub.stopped);

    fixture.pipe->on_disconnection(ec);
}
