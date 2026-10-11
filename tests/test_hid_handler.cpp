#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {
// Test HID handler: returns a fixed report descriptor (5-byte example)
class TestHidHandler : public HidVirtualInterfaceHandler {
public:
    explicit TestHidHandler(UsbInterface &intf, StringPool &pool) : HidVirtualInterfaceHandler(intf, pool) {
    }

    data_type get_report_descriptor() override {
        return {0x05, 0x01, 0x09, 0x06, 0xC0};
    }

    std::uint16_t get_report_descriptor_size() override {
        return 5;
    }

    // Record output reports received by SET_REPORT (keyboard LED scenarios, etc.)
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                            const data_type &data, std::uint32_t *p_status) override {
        last_output_report = data;
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    }

    data_type last_output_report;

    // Public test wrapper forwarding to protected has_pending_input_reports()
    bool has_pending_reports_for_test() const {
        return has_pending_input_reports();
    }

    // For testing: simulate an established connection (channel active, disconnected reset). In real scenarios the device
    // produces reports only after connection; while unconnected, the channel's disconnection check rejects enqueueing (push returns false)
    void connect_for_test() {
        input_channel.on_new_connection();
    }
};

// Construct a minimal usable UsbInterface and StringPool
struct HidTestEnv {
    StringPool pool;
    // Transfer allocator: must be declared before stub (members destroyed in reverse order; op dies later)—
    // RetSubmit entries in submits hold TransferHandle and use op to release it on destruction;
    // op must outlive stub to avoid use-after-scope
    GenericTransferOperator op;
    CaptureResponder stub;
    UsbInterface intf{.interface_class = 3, .interface_subclass = 0, .interface_protocol = 0};
    TestHidHandler handler{intf, pool};
    usbipdcpp::error_code ec;

    // Connect stub: channel-response/data-plane tests use this path (descriptor tests unaffected)
    HidTestEnv() : handler(intf, pool) {
        handler.on_new_connection(stub, ec);
    }
};
} // namespace

// ========== HID descriptors ==========

TEST(VirtualDeviceHandlers, HidGetReportDescriptor) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto desc = env.handler.request_get_descriptor(HidDescriptorType::Report, 0, 64, &status);
    ASSERT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 5u);
    EXPECT_EQ(desc[0], 0x05);
    EXPECT_EQ(desc[4], 0xC0);
}

TEST(VirtualDeviceHandlers, HidGetUnsupportedDescriptorType) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto desc = env.handler.request_get_descriptor(HidDescriptorType::Physical, 0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(desc.empty());
}

TEST(VirtualDeviceHandlers, HidClassSpecificDescriptor) {
    HidTestEnv env;

    auto desc = env.handler.get_class_specific_descriptor();
    // bLength=9, bDescriptorType=0x21(HID), bcdHID=1.11, bCountryCode=0,
    // bNumDescriptors=1, bDescriptorType[0]=0x22(Report), wDescriptorLength=5 (little-endian)
    const data_type expect = {0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x05, 0x00};
    EXPECT_EQ(desc, expect);
}

// ========== Default HID class-request implementation (formal responses match kernel f_hid.c; no stall) ==========

TEST(VirtualDeviceHandlers, HidIdleFormalResponse) {
    HidTestEnv env;

    // GET_IDLE returns initial idle value 0; SET_IDLE accepts unconditionally
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_idle(0, 0, 1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, data_type({0x00}));

    status = 0;
    env.handler.request_set_idle(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
}

TEST(VirtualDeviceHandlers, HidReportFormalResponse) {
    HidTestEnv env;

    // GET_REPORT returns an all-zero empty report (length from host wLength); SET_REPORT accepts and discards
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_report(1, 0, 8, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, data_type(8, 0));

    status = 0;
    env.handler.request_set_report(1, 0, 8, {}, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
}

TEST(VirtualDeviceHandlers, HidProtocolFormalResponse) {
    HidTestEnv env;

    // GET_PROTOCOL returns initial protocol value 0
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_protocol(&status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, 0u);

    // Test interface subclass=0 (not Boot); SET_PROTOCOL should stall (matching the kernel)
    status = 0;
    env.handler.request_set_protocol(1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ========== Default HID standard-request implementation ==========

TEST(VirtualDeviceHandlers, HidStandardRequests) {
    HidTestEnv env;

    std::uint32_t status = 1;
    env.handler.request_clear_feature(0, &status);
    EXPECT_EQ(status, 0u);

    status = 0;
    EXPECT_EQ(env.handler.request_get_interface(&status), 0u);
    EXPECT_EQ(status, 0u);

    status = 0;
    env.handler.request_set_feature(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ========== HID input-report queue ==========

TEST(VirtualDeviceHandlers, HidSendInputReportQueuesWhenNoRequest) {
    HidTestEnv env;
    env.handler.connect_for_test(); // Simulate connection (while unconnected, the disconnection check rejects push)

    EXPECT_FALSE(env.handler.has_pending_reports_for_test());

    // Without queued endpoint requests, reports enter the pending queue
    const std::array<std::uint8_t, 3> report = {1, 2, 3};
    env.handler.send_input_report(asio::buffer(report));
    EXPECT_TRUE(env.handler.has_pending_reports_for_test());

    // Further sends continue queueing
    env.handler.send_input_report(asio::buffer(report));
    EXPECT_TRUE(env.handler.has_pending_reports_for_test());

    // Clear queue on disconnection
    std::error_code ec;
    env.handler.on_disconnection(ec);
    EXPECT_FALSE(env.handler.has_pending_reports_for_test());
}

// ========== HID data plane (interrupt IN pending-response, SET_REPORT control path) ==========

TEST(VirtualDeviceHandlers, HidInterruptInServedBySendInputReport) {
    // Interrupt IN request first stays pending (no report); respond directly when send_input_report pushes one:
    // return report data unchanged, status 0 (HidTestEnv connected to stub responder)
    HidTestEnv env;

    const std::array<std::uint8_t, 4> report = {0x11, 0x22, 0x33, 0x44};
    env.handler.handle_interrupt_transfer(
            1, UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10},
            0, report.size(), make_cmd_submit(env.op, 1, 0x01, UsbIpDirection::In, report.size()).transfer, env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // No report: request pending without a response

    env.handler.send_input_report(asio::buffer(report));
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, report.size());
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type(report.begin(), report.end())));
}

TEST(VirtualDeviceHandlers, HidSetReportDeliveredToOverride) {
    // SET_REPORT (Class|Interface|OUT control request) passes data through request_set_report
    // to the subclass (keyboard LED scenario), returning OK
    HidTestEnv env;

    const SetupPacket setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x09,      // SET_REPORT
            .value = 0x0200,      // Report type 2 (Output), report ID 0
            .index = 0,
            .length = 3,
    };
    const data_type payload = {'L', 'E', 'D'};
    env.handler.handle_non_standard_request_type_control_urb(
            2, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, payload.size(), setup,
            make_cmd_submit(env.op, 2, 0x00, UsbIpDirection::Out, payload.size(), setup, payload).transfer, env.ec);
    ASSERT_FALSE(env.ec);

    EXPECT_EQ(env.handler.last_output_report, payload);
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
}

TEST(VirtualDeviceHandlers, HidSendInputReportDisconnectedRejected) {
    // Without an established connection, the channel's disconnection check rejects send_input_report (avoiding useless accumulation)
    StringPool pool;
    UsbInterface intf{.interface_class = 3, .interface_subclass = 0, .interface_protocol = 0};
    TestHidHandler handler(intf, pool); // on_new_connection not called

    const std::array<std::uint8_t, 2> report = {1, 2};
    handler.send_input_report(asio::buffer(report));
    EXPECT_FALSE(handler.has_pending_reports_for_test());
}

TEST(VirtualDeviceHandlers, HidInputReportQueueDropsOldestOverLimit) {
    // Report buffer exceeding limit (1024) drops the oldest: after sending 1025, report 1 is dropped,
    // and the host IN request retrieves report 2
    HidTestEnv env;

    for (int i = 0; i < 1025; i++) {
        const std::array<std::uint8_t, 2> report = {static_cast<std::uint8_t>(i & 0xFF),
                                                    static_cast<std::uint8_t>((i >> 8) & 0xFF)};
        env.handler.send_input_report(asio::buffer(report));
    }

    env.handler.handle_interrupt_transfer(
            1, UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10},
            0, 2, make_cmd_submit(env.op, 1, 0x01, UsbIpDirection::In, 2).transfer, env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{1, 0})); // Report 2
}
