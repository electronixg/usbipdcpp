#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/MemoryBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageTransferOperator.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// MSC device + handler pointer (handler owned by device, same lifetime as device)
struct MscFixture {
    std::shared_ptr<UsbDevice> device;
    MscBulkOnlyHandler *msc = nullptr;
    UsbEndpoint ep_in;
    UsbEndpoint ep_out;
};

// Construct MSC device (matching examples/mock_msc): Mass Storage + SCSI + BOT,
// bulk IN 0x81 + bulk OUT 0x02,High speed
MscFixture make_msc_device(StringPool &string_pool, std::unique_ptr<StorageBackend> backend,
                           bool read_only = false, MscConfig config = {}) {
    std::vector<UsbInterface> interfaces = {MscBulkOnlyHandler::make_interface(0x81, 0x02)};
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_msc",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5691,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });
    auto msc = device->interfaces[0].with_handler<MscBulkOnlyHandler>(string_pool, std::move(backend),
                                                                      std::move(config), read_only);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    auto &intf = device->interfaces[0];
    return {device, msc.get(), intf.endpoints[0][0], intf.endpoints[0][1]};
}

// Construct CBW byte stream (BOT protocol, little-endian fields, memcpy the struct directly)
data_type make_cbw(std::uint32_t tag, std::uint8_t flags, const std::array<std::uint8_t, 16> &cdb,
                   std::uint32_t transfer_len) {
    CBW cbw{};
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = transfer_len;
    cbw.bmCBWFlags = flags;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = 16;
    std::memcpy(cbw.CBWCB, cdb.data(), 16);
    data_type bytes(sizeof(CBW));
    std::memcpy(bytes.data(), &cbw, sizeof(CBW));
    return bytes;
}

// READ10 / WRITE10 CDB (LBA and block count big-endian)
std::array<std::uint8_t, 16> read10_cdb(std::uint32_t lba, std::uint16_t blocks) {
    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x28; // READ(10)
    cdb[2] = static_cast<std::uint8_t>((lba >> 24) & 0xFF);
    cdb[3] = static_cast<std::uint8_t>((lba >> 16) & 0xFF);
    cdb[4] = static_cast<std::uint8_t>((lba >> 8) & 0xFF);
    cdb[5] = static_cast<std::uint8_t>(lba & 0xFF);
    cdb[7] = static_cast<std::uint8_t>((blocks >> 8) & 0xFF);
    cdb[8] = static_cast<std::uint8_t>(blocks & 0xFF);
    return cdb;
}

std::array<std::uint8_t, 16> write10_cdb(std::uint32_t lba, std::uint16_t blocks) {
    auto cdb = read10_cdb(lba, blocks);
    cdb[0] = 0x2A; // WRITE(10)
    return cdb;
}

// Simulate network layer receiving OUT data (matching StorageTransferOperator::recv_transfer_data
// fallback path: prepare_out_buffer sets external_buf during alloc, then calls
// on_out_data_received after reception). For CBW (Idle), external_buf is empty, using fallback_data
void deliver_out_data(MscBulkOnlyHandler &msc, std::uint32_t seqnum, const data_type &payload) {
    // Call virtual allocation through a base-class pointer: avoid polymorphic downcasting of StorageTransferOperator via
    // static_cast (references its RTTI on Linux linking; resolution fails in some build configurations)
    auto *op = msc.get_transfer_operator();
    UsbIpHeaderBasic header{};
    header.direction = UsbIpDirection::Out;
    auto *trx = StorageIoTransfer::from_handle(op->alloc_transfer_handle(payload.size(), 0, header, {}));
    if (trx->external_buf) {
        std::memcpy(trx->external_buf, payload.data(), payload.size());
    }
    else {
        trx->fallback_data = payload;
    }
    msc.on_out_data_received(trx, payload.size());
}

// Host IN request: transfer must be allocated by the interface op (handle_bulk_transfer casts to
// StorageIoTransfer to write external_buf); cannot use make_cmd_submit's GenericTransfer
void deliver_in_request(MscBulkOnlyHandler &msc, std::uint32_t seqnum, std::uint32_t length, const UsbEndpoint &ep_in,
                        usbipdcpp::error_code &ec) {
    auto *op = msc.get_transfer_operator();
    UsbIpHeaderBasic header{};
    header.direction = UsbIpDirection::In;
    auto *trx = StorageIoTransfer::from_handle(op->alloc_transfer_handle(length, 0, header, {}));
    TransferHandle handle(trx, op);
    msc.handle_bulk_transfer(seqnum, ep_in, 0, length, std::move(handle), ec);
}

// Read IN data from RET_SUBMIT (external_buf points to staging or mmap)
data_type data_from_submit(const UsbIpResponse::UsbIpRetSubmit &ret) {
    auto *trx = StorageIoTransfer::from_handle(ret.transfer.get());
    const auto *p = static_cast<const std::uint8_t *>(trx->external_buf);
    return data_type(p, p + ret.actual_length);
}

// Read CSW from RET_SUBMIT (Status-phase response, data in fallback_data)
CSW csw_from_submit(const UsbIpResponse::UsbIpRetSubmit &ret) {
    auto *trx = StorageIoTransfer::from_handle(ret.transfer.get());
    CSW csw{};
    std::memcpy(&csw, trx->fallback_data.data(), sizeof(CSW));
    return csw;
}

} // namespace

TEST(TestMscHandler, Read10ServesBlocksThenCsw) {
    // READ10: CBW → DataIn (mmap zero-copy path; external_buf points directly to backend buffer) →
    // CSW (status 0, residue 0). Matches BOT protocol state machine
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(1024);
    for (std::size_t i = 0; i < pattern.size(); i++) {
        pattern[i] = static_cast<std::uint8_t>(i);
    }
    std::memcpy(backend->get_direct_buffer(2), pattern.data(), pattern.size()); // Prefill LBA 2-3
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // CBW: READ10 LBA=2, read 2 blocks (1024 bytes), IN direction
    deliver_out_data(*fixture.msc, 1, make_cbw(0x1234, 0x80, read10_cdb(2, 2), 1024));

    // Host IN request retrieves data
    deliver_in_request(*fixture.msc, 2, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 1024u);
    EXPECT_EQ(data_from_submit(stub.submits[0]), pattern);

    // Host IN request retrieves CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].header.seqnum, 3u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(stub.submits[1].actual_length, 13u);
    auto csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.dCSWSignature, CSW_SIGNATURE);
    EXPECT_EQ(csw.dCSWTag, 0x1234u);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u); // passed

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10WritesBackendThenCsw) {
    // WRITE10: CBW → DataOut (data enters staging/mmap) → CSW. Backend content changes after disk write
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // CBW: WRITE10 LBA=1, write 1 block (512 bytes), OUT direction
    deliver_out_data(*fixture.msc, 1, make_cbw(0xABCD, 0x00, write10_cdb(1, 1), 512));

    // DataOut: 512 bytes of data
    data_type payload(512);
    for (std::size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<std::uint8_t>(i * 3 + 7);
    }
    deliver_out_data(*fixture.msc, 2, payload);

    // Host IN request retrieves CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 3u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWSignature, CSW_SIGNATURE);
    EXPECT_EQ(csw.dCSWTag, 0xABCDu);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    // Backend LBA 1 content updated
    auto *direct = fixture.msc->get_backend()->get_direct_buffer(1);
    EXPECT_EQ(std::memcmp(direct, payload.data(), payload.size()), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InvalidCbwSignatureReportsFailedCsw) {
    // Invalid CBW signature: enter no command branch, directly CSW status=1 (command failed),
    // residue = bytes expected but not transferred (= dCBWDataTransferLength)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto bad_cbw = make_cbw(0x7777, 0x80, read10_cdb(0, 1), 512);
    bad_cbw[0] = 0xDE;
    bad_cbw[1] = 0xAD;
    bad_cbw[2] = 0xBE;
    bad_cbw[3] = 0xEF;
    deliver_out_data(*fixture.msc, 1, bad_cbw);

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x7777u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, TestUnitReadyCompletesWithoutDataStage) {
    // TEST UNIT READY: no data phase; directly output CSW (passed) in Status
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::TestUnitReady;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x1111, 0x00, cdb, 0));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x1111u);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InRequestBeforeCbwStalls) {
    // IN request before receiving CBW (Idle state): return EPIPE (state machine does not recognize this phase)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_in_request(*fixture.msc, 1, 512, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, OutRequestAcknowledgedWithoutData) {
    // Endpoint OUT request (non-CBW path): acknowledge without consuming (BOT OUT data handling uses
    // prepare_out_buffer/on_out_data_received through network receive callbacks, not this path)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    fixture.msc->handle_bulk_transfer(1, fixture.ep_out, 0, 31, TransferHandle{}, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 31u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10OnReadOnlyDeviceFailsCsw) {
    // Read-only device receives WRITE10: parsing CBW marks command failed, CSW status=1 (matching kernel
    // fsg rejection of read-only media); no reception in data phase
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8), /*read_only=*/true);

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x2222, 0x00, write10_cdb(1, 1), 512));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x2222u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u); // Data phase not transferred
    EXPECT_EQ(csw.bCSWStatus, 1u);        // failed

    // Backend content not modified
    data_type backend_data(512, 0);
    auto *direct = fixture.msc->get_backend()->get_direct_buffer(1);
    EXPECT_EQ(std::memcmp(direct, backend_data.data(), 512), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, RequestSenseReturnsSenseData) {
    // REQUEST SENSE: fixed-format sense data (0x70 + additional length 10) returned through DataIn,
    // matching kernel fsg response structure
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::RequestSense;
    cdb[7] = 18; // Allocation length
    deliver_out_data(*fixture.msc, 1, make_cbw(0x3333, 0x80, cdb, 18));

    deliver_in_request(*fixture.msc, 2, 18, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 18u);
    auto sense = data_from_submit(stub.submits[0]);
    ASSERT_EQ(sense.size(), 18u);
    EXPECT_EQ(sense[0], 0x70);         // Fixed format, valid
    EXPECT_EQ(sense[7], 10);           // Additional length
    EXPECT_EQ(sense[12], 0);           // ASC
    EXPECT_EQ(sense[13], 0);           // ASCQ

    // CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    auto csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.dCSWTag, 0x3333u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

// ========== MSC edge cases ==========

TEST(TestMscHandler, InquiryStandardReturnsIdentifiers) {
    // Standard INQUIRY: fixed fields (device type/removable media/SPC-4) + vendor/product/revision
    // (from MscConfig; on_setup_interface_handlers fills defaults)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8), false,
                                   MscConfig{.vendor = "TestVend", .product = "TestProduct1234",
                                             .revision = "1.2", .serial = "SN001"});

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[4] = 36;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4444, 0x80, cdb, 36));

    deliver_in_request(*fixture.msc, 2, 36, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 36u);
    auto data = data_from_submit(stub.submits[0]);
    EXPECT_EQ(data[0], 0x00); // Direct-access block device
    EXPECT_EQ(data[1], 0x80); // Removable media
    EXPECT_EQ(data[2], 0x07); // SPC-4
    EXPECT_EQ(data[3], 0x12); // HiSup=1, response format 2
    EXPECT_EQ(data[4], 31);   // Additional length
    EXPECT_EQ(std::string(data.begin() + 8, data.begin() + 16), "TestVend"); // Exactly 8 characters
    EXPECT_EQ(std::string(data.begin() + 16, data.begin() + 32), "TestProduct1234 "); // 15 characters padded with 1 space
    EXPECT_EQ(std::string(data.begin() + 32, data.begin() + 36), "1.2 ");

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacity10ReportsCapacity) {
    // READ CAPACITY(10): last LBA and block size (big-endian)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity10;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5555, 0x80, cdb, 8));

    deliver_in_request(*fixture.msc, 2, 8, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 8u);
    auto data = data_from_submit(stub.submits[0]);
    EXPECT_EQ(data, (data_type{0, 0, 0, 7, 0, 0, 2, 0})); // last_lba=7, block_size=512

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Read10BeyondCapacityFails) {
    // READ10 out of bounds (LBA+count > capacity): command failed, CSW status=1
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x6666, 0x80, read10_cdb(7, 2), 1024));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x6666u);
    EXPECT_EQ(csw.dCSWDataResidue, 1024u);
    EXPECT_EQ(csw.bCSWStatus, 1u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Read10AcrossMultipleInRequests) {
    // Large read across multiple IN requests: staging_offset_ resumes transmission; concatenated segments match backend content,
    // transitioning to Status only after all sent (this scenario uses mmap zero-copy; external_buf points to backend buffer)
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(2048);
    for (std::size_t i = 0; i < pattern.size(); i++) {
        pattern[i] = static_cast<std::uint8_t>(i * 7);
    }
    std::memcpy(backend->get_direct_buffer(0), pattern.data(), pattern.size());
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x7777, 0x80, read10_cdb(0, 4), 2048));

    deliver_in_request(*fixture.msc, 2, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    deliver_in_request(*fixture.msc, 3, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[0].actual_length, 1024u);
    EXPECT_EQ(stub.submits[1].actual_length, 1024u);
    auto first = data_from_submit(stub.submits[0]);
    auto second = data_from_submit(stub.submits[1]);
    first.insert(first.end(), second.begin(), second.end());
    EXPECT_EQ(first, pattern);

    // All data sent → CSW
    deliver_in_request(*fixture.msc, 4, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(csw_from_submit(stub.submits[2]).bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10AcrossMultipleOutRequests) {
    // Large write across multiple OUT requests: data enters mmap (zero-copy path); accumulate declared length
    // before disk write and transition to Status. Backend content matches concatenated data
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x8888, 0x00, write10_cdb(0, 2), 1024));

    data_type first_half(512, 0x11);
    data_type second_half(512, 0x22);
    deliver_out_data(*fixture.msc, 2, first_half);
    deliver_out_data(*fixture.msc, 3, second_half);

    deliver_in_request(*fixture.msc, 4, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    auto *direct = fixture.msc->get_backend()->get_direct_buffer(0);
    EXPECT_EQ(std::memcmp(direct, first_half.data(), 512), 0);
    EXPECT_EQ(std::memcmp(static_cast<char *>(direct) + 512, second_half.data(), 512), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnmapPunchesHoles) {
    // UNMAP: DataOut receives descriptor list (8-byte header + one block descriptor per 16 bytes),
    // clearing backend segment by segment using punch_hole
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(512, 0xFF);
    for (std::uint64_t lba = 0; lba < 8; lba++) {
        std::memcpy(backend->get_direct_buffer(lba), pattern.data(), 512);
    }
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Unmap;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x9999, 0x00, cdb, 40));

    // 40 bytes: 8-byte header + 2 descriptors (LBA=2 cnt=1, LBA=5 cnt=2).
    // Big-endian fields: LBA low byte at end of offset range (offset+7); cnt low byte at offset+11
    data_type unmap_data(40, 0);
    unmap_data[15] = 2;                 // Descriptor 1: LBA=2 (be64 low byte at [15])
    unmap_data[19] = 1;                 // Descriptor 1: cnt=1 (be32 low byte at [19])
    unmap_data[31] = 5;                 // Descriptor 2: LBA=5
    unmap_data[35] = 2;                 // Descriptor 2: cnt=2
    deliver_out_data(*fixture.msc, 2, unmap_data);

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    data_type zero(512, 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(2), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(5), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(6), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(7), pattern.data(), 512), 0); // Unchanged

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, WriteSameFillsRange) {
    // WRITE SAME(16): DataOut receives 1 block of fill data, writing the entire range block by block (3 blocks starting at LBA=1)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x93; // WRITE SAME(16)
    cdb[9] = 1;    // LBA=1 (be64, low byte at [9])
    cdb[13] = 3;   // cnt=3 (be32, low byte at [13])
    deliver_out_data(*fixture.msc, 1, make_cbw(0xAAAA, 0x00, cdb, 512));

    data_type fill(512, 0x5A);
    deliver_out_data(*fixture.msc, 2, fill);

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    for (std::uint64_t lba = 1; lba <= 3; lba++) {
        EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(lba), fill.data(), 512), 0);
    }
    data_type zero(512, 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(0), zero.data(), 512), 0); // Unchanged

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnsupportedCommandFails) {
    // Unknown SCSI command: command failed, CSW status=1 (no data phase)
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x7F;
    deliver_out_data(*fixture.msc, 1, make_cbw(0xBBBB, 0x80, cdb, 1024));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0xBBBBu);
    EXPECT_EQ(csw.bCSWStatus, 1u);

    fixture.msc->on_disconnection(ec);
}
