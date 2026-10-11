// Microsoft OS 1.0 descriptor tests: 0xEE signature string ("MSFT100") and Compatible ID feature
// descriptor. During enumeration, Windows first probes MS OS support with the 0xEE string, then
// requests Compatible ID using bMS_VendorCode to load a built-in driver (e.g. RNDIS → rndismp)

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/MsOsConstants.h"
#include "usbipdcpp/virtual_device/RndisMsOsDeviceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// Expose protected protocol handlers as public so tests can submit control requests directly
class TestableMsOsHandler : public MSOSSimpleVirtualDeviceHandler {
public:
    using MSOSSimpleVirtualDeviceHandler::MSOSSimpleVirtualDeviceHandler;
    using MSOSSimpleVirtualDeviceHandler::get_special_string_descriptor;
    using MSOSSimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb;
};

// Device with device-level MS OS support (matching binding in examples/mock_rndis)
struct MsOsFixture {
    // Transfer allocator: must be declared before stub (members destroyed in reverse order; op dies later)
    GenericTransferOperator op;
    StringPool pool;
    CaptureResponder stub;
    std::shared_ptr<UsbDevice> device;
    TestableMsOsHandler *dev = nullptr;
    usbipdcpp::error_code ec;

    MsOsFixture() {
        std::vector<UsbInterface> interfaces = {
                {.interface_class = 0x02,
                 .interface_subclass = 0x02,
                 .interface_protocol = 0xFF,
                 .endpoints = {}}};
        device = UsbDevice::make("1-1", 0x1234, 0x56E2, std::move(interfaces), 1, 1,
                                 static_cast<std::uint8_t>(ClassCode::CDC));
        dev = device->with_handler<TestableMsOsHandler>(pool).get();
        dev->setup_interface_handlers();
        // Bind responder: unattached interface handlers are harmless (on_new_connection skips interfaces without
        // handlers); failures outside this test's scope; error retained in ec member
        dev->on_new_connection(stub, ec);
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

// Submit an IN-direction device-level control request (shaped as an MS OS Compatible ID request)
void deliver_ms_os_request(MsOsFixture &fx, std::uint32_t seqnum, const SetupPacket &setup) {
    auto submit = make_cmd_submit(fx.op, seqnum, 0x80, UsbIpDirection::In, setup.length, setup);
    fx.dev->handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, setup.length, setup, std::move(submit.transfer),
            fx.ec);
    ASSERT_FALSE(fx.ec);
}

// Standard MS OS Compatible ID request: Vendor|Device|IN(0xC0) + bRequest=0x01 +
// wValue=0x0000 + wIndex=0x0004 + wLength=40
SetupPacket make_compat_id_request(std::uint8_t request = MS_OS_DEFAULT_VENDOR_CODE, std::uint16_t value = 0,
                                   std::uint16_t index = MS_OS_COMPAT_ID_WINDEX, std::uint16_t length = 40) {
    return {.request_type = 0xC0, .request = request, .value = value, .index = index, .length = length};
}

} // namespace

// ========== 0xEE signature string (GET_DESCRIPTOR String) ==========

TEST(TestMsOsHandler, SpecialStringNotConfiguredFallsThrough) {
    // Compatible ID unconfigured = no MS OS descriptors: 0xEE uses default (nullopt)
    MsOsFixture fx;
    EXPECT_EQ(fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX), std::nullopt);
    EXPECT_EQ(fx.dev->get_special_string_descriptor(1), std::nullopt);
}

TEST(TestMsOsHandler, SpecialStringReturnsMsftSignature) {
    // Once configured, 0xEE returns an 18-byte signature string: bLength=18, type=0x03 STRING,
    // "MSFT100" UTF-16LE, bMS_VendorCode, bPad=0 (matching kernel composite.c
    // usb_os_string layout)
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");

    auto desc = fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->size(), sizeof(MsOsStringDesc));
    EXPECT_EQ((*desc)[0], 18u);
    EXPECT_EQ((*desc)[1], static_cast<std::uint8_t>(DescriptorType::String));
    const data_type expected_sig = {
            0x4D, 0x00, 0x53, 0x00, 0x46, 0x00, 0x54, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00,
    };
    EXPECT_TRUE(std::equal(expected_sig.begin(), expected_sig.end(), desc->begin() + 2));
    EXPECT_EQ((*desc)[16], MS_OS_DEFAULT_VENDOR_CODE);
    EXPECT_EQ((*desc)[17], 0u);
    // Other indices unaffected
    EXPECT_EQ(fx.dev->get_special_string_descriptor(1), std::nullopt);
}

TEST(TestMsOsHandler, SpecialStringUsesCustomVendorCode) {
    // After set_ms_os_vendor_code, bMS_VendorCode in signature string changes accordingly
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_vendor_code(0x21);

    auto desc = fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ((*desc)[16], 0x21u);
}

// ========== Compatible ID feature descriptor (Vendor request) ==========

TEST(TestMsOsHandler, CompatIdRequestReturnsDescriptor) {
    // Full request → 40-byte descriptor (16-byte header + 24-byte function section), fields match kernel
    // composite.c fill_ext_compat layout
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");

    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    ASSERT_EQ(ret->actual_length, 40u);

    auto data = ret_submit_data(*ret);
    ASSERT_EQ(data.size(), 40u);
    // Header: dwLength=40, bcdVersion=0x0100, wIndex=0x0004, bCount=1, Reserved[7]=0
    EXPECT_EQ(read_le32_at(data, 0), 40u);
    EXPECT_EQ(read_le16_at(data, 4), 0x0100u);
    EXPECT_EQ(read_le16_at(data, 6), 0x0004u);
    EXPECT_EQ(data[8], 1u);
    EXPECT_TRUE(std::all_of(data.begin() + 9, data.begin() + 16, [](std::uint8_t b) { return b == 0; }));
    // Function section: bFirstInterface=0, Reserved1=0x01 (matching kernel fixed value),
    // CompatibleID="RNDIS"+3 zeros, SubCompatibleID all zero, Reserved2 all zero
    EXPECT_EQ(data[16], 0u);
    EXPECT_EQ(data[17], 0x01u);
    const std::string compat(reinterpret_cast<const char *>(data.data() + 18), 8);
    EXPECT_EQ(compat, std::string("RNDIS\0\0\0", 8));
    EXPECT_TRUE(std::all_of(data.begin() + 26, data.begin() + 40, [](std::uint8_t b) { return b == 0; }));
}

TEST(TestMsOsHandler, CompatIdNotConfiguredStalls) {
    // Compatible ID unconfigured: all MS OS support disabled; request uses parent-class EPIPE
    MsOsFixture fx;
    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(ret->actual_length, 0u);
}

TEST(TestMsOsHandler, CompatIdWrongIndexStalls) {
    // wIndex ≠ 0x0004 (e.g. Extended Properties 0x0005): outside this class's responsibility, EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 0, 0x0005));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdWrongValueStalls) {
    // Nonzero low byte of wValue (1 = Extended Properties request): unsupported, EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 1));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdOutDirectionStalls) {
    // OUT direction (0x40 rather than 0xC0): Compatible ID can only be an IN request, EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    SetupPacket setup = make_compat_id_request();
    setup.request_type = 0x40; // Vendor | Device | OUT
    deliver_ms_os_request(fx, 1, setup);
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdWrongRequestStalls) {
    // bRequest ≠ bMS_VendorCode (e.g. undefined request 0x00): EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(0x00));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdUsesCustomVendorCode) {
    // Custom bMS_VendorCode: respond normally to matching request number; old number gets EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_vendor_code(0x21);

    deliver_ms_os_request(fx, 1, make_compat_id_request(0x21));
    EXPECT_EQ(fx.find_submit(1)->status, 0u);

    deliver_ms_os_request(fx, 2, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE));
    EXPECT_EQ(fx.find_submit(2)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdTruncatedByBufferLength) {
    // Host wLength less than 40 (truncate to buffer, matching get_string_descriptor truncation behavior)
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 0, MS_OS_COMPAT_ID_WINDEX, 20));
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    ASSERT_EQ(ret->actual_length, 20u);
    EXPECT_EQ(ret_submit_data(*ret).size(), 20u);
}

TEST(TestMsOsHandler, CompatIdIncludesSubCompatibleId) {
    // SubCompatibleID: Windows driver matching may depend on it (rndismp matches
    // MS_SUBCOMP_5162001; see rndiscmp.inf); empty value causes matching failure
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_sub_compatible_id("5162001");
    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    ASSERT_EQ(ret->actual_length, 40u);
    auto data = ret_submit_data(*ret);
    ASSERT_EQ(data.size(), 40u);
    const std::string sub(reinterpret_cast<const char *>(data.data() + 26), 8);
    EXPECT_EQ(sub, std::string("5162001\0", 8));
}

// Compile-time assertion: RndisMsOsDeviceHandler configuration setters hidden as private via using
// (RNDIS preset is part of the functional definition; external changes not allowed)
template <typename T, typename = void>
struct CanCallSetCompatibleId : std::false_type {};
template <typename T>
struct CanCallSetCompatibleId<T, std::void_t<decltype(std::declval<T &>().set_ms_os_compatible_id(""))>>
        : std::true_type {};

TEST(TestMsOsHandler, RndisDeviceHandlerPresets) {
    // RndisMsOsDeviceHandler (final) presets RNDIS + 5162001 (part of the functional definition),
    // assembling an RNDIS device in one line; complete Compatible ID response bytes covered by
    // CompatIdIncludesSubCompatibleId (base class manually configured with same values)
    MsOsFixture fx;
    RndisMsOsDeviceHandler rndis{*fx.device, fx.pool};
    EXPECT_EQ(rndis.get_ms_os_compatible_id(), "RNDIS");
    EXPECT_EQ(rndis.get_ms_os_sub_compatible_id(), "5162001");
    EXPECT_FALSE(CanCallSetCompatibleId<RndisMsOsDeviceHandler>::value);
}
