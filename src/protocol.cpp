#include "usbipdcpp/protocol.h"

#include <algorithm>
#include <filesystem>

#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <variant>
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/utils/SmallVector.h"


using namespace usbipdcpp;

// ========== TransferHandle implementation ==========

TransferHandle::TransferHandle(void *handle, TransferOperator *op) : handle_(handle), op_(op) {
}

TransferHandle::TransferHandle(TransferHandle &&other) noexcept : handle_(other.handle_), op_(other.op_) {
    other.handle_ = nullptr;
    other.op_ = nullptr;
}

TransferHandle &TransferHandle::operator=(TransferHandle &&other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        op_ = other.op_;
        other.handle_ = nullptr;
        other.op_ = nullptr;
    }
    return *this;
}

TransferHandle::~TransferHandle() {
    reset();
}

void TransferHandle::reset() {
    if (handle_ && op_) {
        op_->free_transfer_handle(handle_);
    }
    handle_ = nullptr;
    op_ = nullptr;
}

void *TransferHandle::release() {
    void *tmp = handle_;
    handle_ = nullptr;
    op_ = nullptr;
    return tmp;
}

// ========== Other protocol implementations ==========

const usbipdcpp::TransferErrorCategory g_error_category;

const char *usbipdcpp::TransferErrorCategory::name() const noexcept {
    return "UsbIp Error Category";
}

std::string usbipdcpp::TransferErrorCategory::message(int _Errval) const {
    auto e = static_cast<ErrorType>(_Errval);
    switch (e) {
        case ErrorType::OK: {
            return "OK";
        }
        case ErrorType::UNKNOWN_VERSION: {
            return "Unknown UsbIp Version";
        }
        case ErrorType::UNKNOWN_CMD: {
            return "Unknown Command";
        }
        case ErrorType::PROTOCOL_ERROR: {
            return "Protocol Error";
        }
        case ErrorType::NO_DEVICE: {
            return "No Device";
        }
        case ErrorType::SOCKET_EOF: {
            return "Connection closed by peer";
        }
        case ErrorType::SOCKET_ERR: {
            return "Connection err";
        }
        case ErrorType::INTERNAL_ERROR: {
            return "Internal Error";
        }
        case ErrorType::INVALID_ARG: {
            return "Invalid Argument";
        }
        case ErrorType::UNIMPLEMENTED: {
            return "Unimplemented";
        }
        case ErrorType::TRANSFER_ERROR: {
            return "Transfer Error";
        }
        default:;
            return "Unknown Error";
    }
}


std::error_code usbipdcpp::make_error_code(ErrorType e) {
    return {static_cast<int>(e), g_error_category};
}

array_data_type<usbipdcpp::calculate_total_size_with_array<
        decltype(UsbIpHeaderBasic::command), decltype(UsbIpHeaderBasic::seqnum), decltype(UsbIpHeaderBasic::devid),
        decltype(UsbIpHeaderBasic::direction), decltype(UsbIpHeaderBasic::ep)>()>
UsbIpHeaderBasic::to_bytes() const {
    return to_network_array(command, seqnum, devid, direction, ep);
}

void UsbIpHeaderBasic::from_socket(asio::ip::tcp::socket &sock) {
    unsigned_integral_read_from_socket(sock, seqnum, devid, direction, ep);
}

array_data_type<calculate_total_size_with_array<
        decltype(UsbIpIsoPacketDescriptor::offset), decltype(UsbIpIsoPacketDescriptor::length),
        decltype(UsbIpIsoPacketDescriptor::actual_length), decltype(UsbIpIsoPacketDescriptor::status)>()>
usbipdcpp::UsbIpIsoPacketDescriptor::to_bytes() const {
    return to_network_array(offset, length, actual_length, status);
}

void usbipdcpp::UsbIpIsoPacketDescriptor::from_socket(asio::ip::tcp::socket &sock) {
    unsigned_integral_read_from_socket(sock, offset, length, actual_length, status);
}

std::vector<std::uint8_t> usbipdcpp::UsbIpResponse::OpRepDevlist::to_bytes() const {
    std::vector<std::uint8_t> result = to_network_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count);
    for (auto &device: devices) {
        auto bytes = device.to_bytes_with_interfaces();
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

void UsbIpResponse::OpRepDevlist::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_network_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count)), ec);
    for (auto &device: devices) {
        asio::write(sock, asio::buffer(device.to_bytes_with_interfaces()), ec);
    }
}

void UsbIpResponse::OpRepDevlist::from_socket(asio::ip::tcp::socket &sock) {
    // Symmetric with to_socket: status + device_count + each device (312-byte fixed header +
    // interface-count 4-byte interface bodies; the count is at the end of the device header). The version/command
    // fields are consumed by the caller when reading the command code and are not read here
    data_read_from_socket(sock, status, device_count);
    devices.clear();
    // device_count comes from the untrusted network: a malicious server can send a huge value, causing reserve to throw
    // bad_alloc and crash (the same defensive approach as the size checks in CmdSubmit::from_socket)
    devices.reserve(std::min<std::uint32_t>(device_count, 256));
    for (std::uint32_t i = 0; i < device_count; i++) {
        UsbDevice device;
        device.from_socket(sock);
        // Interface bodies immediately follow the device header (the to_bytes_with_interfaces format),
        // with their number given by the interface count at the end of the header
        for (auto &intf: device.interfaces) {
            intf.from_socket(sock);
        }
        devices.push_back(std::move(device));
    }
}


usbipdcpp::UsbIpResponse::OpRepDevlist
usbipdcpp::UsbIpResponse::OpRepDevlist::create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices) {
    std::vector<UsbDevice> ret_devices;
    ret_devices.reserve(devices.size());
    for (auto &device: devices) {
        ret_devices.emplace_back(*device);
    }
    return {.status = 0, .device_count = static_cast<uint32_t>(ret_devices.size()), .devices = std::move(ret_devices)};
}

std::vector<std::uint8_t> usbipdcpp::UsbIpResponse::OpRepImport::to_bytes() const {
    std::vector<std::uint8_t> result = to_network_data(USBIP_VERSION, OP_REP_IMPORT, status);
    if (status == 0) {
        if (device) {
            vector_append_to_net(result, device->to_bytes());
        }
    }
    return result;
}

void UsbIpResponse::OpRepImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    std::array<asio::const_buffer, 2> buffers;
    auto data1 = to_network_array(USBIP_VERSION, OP_REP_IMPORT, status);
    buffers[0] = asio::buffer(data1);
    if (status == 0) {
        if (device) {
            auto data2 = device->to_bytes();
            buffers[1] = asio::buffer(data2);
            asio::write(sock, buffers, ec);
        }
        else {
            asio::write(sock, buffers[0], ec);
        }
    }
    else {
        asio::write(sock, buffers[0], ec);
    }
}

void UsbIpResponse::OpRepImport::from_socket(asio::ip::tcp::socket &sock) {
    // Symmetric with to_socket: status is followed (on success) by the device descriptor (312-byte fixed header,
    // without interface bodies; the server's import response sends only to_bytes()). version/command
    // are consumed by the caller when reading the command code
    data_read_from_socket(sock, status);
    if (status == 0) {
        device = std::make_shared<UsbDevice>();
        device->from_socket(sock);
    }
}

usbipdcpp::UsbIpResponse::OpRepImport
usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure_with_status(std::uint32_t status) {
    return {.status = status, .device = {}};
}

usbipdcpp::UsbIpResponse::OpRepImport usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure() {
    return create_on_failure_with_status(static_cast<std::uint32_t>(OperationStatuType::NA));
}

usbipdcpp::UsbIpResponse::OpRepImport
usbipdcpp::UsbIpResponse::OpRepImport::create_on_success(std::shared_ptr<UsbDevice> device) {
    return {.status = static_cast<std::uint32_t>(OperationStatuType::OK), .device = std::move(device)};
}


void UsbIpResponse::UsbIpRetSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    assert(header.command == USBIP_RET_SUBMIT);

    auto data1 = array_add_padding<8>(
            to_network_array(header.to_bytes(), status, actual_length, start_frame, number_of_packets, error_count));

    // Get data from transfer. The entry condition cannot consider actual_length alone: even when an ISO transfer's
    // actual_length is 0 (e.g. all packets failed/0 bytes), the operator must send the iso
    // descriptors (vhci reads number_of_packets descriptors; omitting them misaligns the stream). The data-send
    // length is calculated according to transfer_is_in: IN uses actual_length (returning data), while OUT
    // always uses 0 (OUT responses return no data; see the usb_pipein condition in stub_tx.c; vhci
    // also reads no data for OUT, so a nonzero length makes it misread data as iso descriptors and fail validation).
    // For non-ISO transfers with actual_length 0, take the else branch and send only the header; do not put
    // 0-byte non-ISO transfers through the op path (e.g. the storage backend's zero-copy send_direct calls
    // TransmitFile(0) on Windows, which sends the entire file)
    if (transfer && (actual_length > 0 || number_of_packets > 0)) {
        auto *op = transfer.get_operator();
        void *raw_handle = transfer.get();

        asio::write(sock, asio::buffer(data1), ec);
        if (!ec)
            op->send_transfer_data(raw_handle, sock, op->transfer_is_in(raw_handle) ? actual_length : 0, ec);
    }
    else {
        asio::write(sock, asio::buffer(data1), ec);
    }
}

void UsbIpResponse::UsbIpRetSubmit::from_socket(asio::ip::tcp::socket &sock) {
    // Symmetric with to_socket: the header and transfer parameters total 40 bytes, followed by 8 bytes of padding,
    // making up the kernel usbip_header's 48 bytes (vhci_rx_pdu reads sizeof(usbip_header) at once;
    // ret_submit occupies 20 bytes in the union, and this project zero-fills the remaining 8 bytes).
    // command is set by this function, not read
    unsigned_integral_and_array_read_from_socket<8>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                    status, actual_length, start_frame, number_of_packets,
                                                    error_count);
    header.command = USBIP_RET_SUBMIT;

    // The data stage is not read here: the RET_SUBMIT header contains no direction information (the server clears it in responses),
    // so the presence of a data stage cannot be determined; for OUT transfers, actual_length is only the number of bytes received,
    // and the kernel stub returns no data in OUT responses (stub_tx.c sends the buffer only for usb_pipein).
    // The kernel vhci matches its own CMD_SUBMIT using seqnum and decides whether to read data based on the URB direction
    // (usbip_recv_xbuff skips OUT directly). Callers needing a data stage should explicitly read it according to their own
    // transfer direction through the transfer's recv_transfer_data
}


usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit(std::uint32_t seqnum, std::uint32_t status,
                                                            std::uint32_t actual_length, std::uint32_t start_frame,
                                                            std::uint32_t number_of_packets, TransferHandle transfer) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = start_frame,
                              .number_of_packets = number_of_packets,
                              .error_count = 0,
                              .transfer = std::move(transfer)};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(std::uint32_t seqnum,
                                                                            std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(std::uint32_t seqnum,
                                                                                    std::uint32_t status,
                                                                                    std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum,
                                                                                   std::uint32_t status,
                                                                                   std::uint32_t actual_length,
                                                                                   TransferHandle transfer) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0,
                              .transfer = std::move(transfer)};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    actual_length, std::move(transfer));
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(std::uint32_t seqnum,
                                                                               std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    actual_length, std::move(transfer));
}

array_data_type<calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()),
                                                decltype(UsbIpResponse::UsbIpRetUnlink::status)>() +
                24>
UsbIpResponse::UsbIpRetUnlink::to_bytes() const {
    assert(header.command == USBIP_RET_UNLINK);
    auto result = to_network_array(header.to_bytes(), status);
    return array_add_padding<24>(result);
}

void UsbIpResponse::UsbIpRetUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    SPDLOG_DEBUG("RET_UNLINK to_socket: seqnum={} status={}", header.seqnum, status);
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpResponse::UsbIpRetUnlink::from_socket(asio::ip::tcp::socket &sock) {
    // Symmetric with to_bytes: 20-byte header + 4-byte status + 24-byte padding,
    // making up the kernel usbip_header's 48 bytes (ret_unlink occupies only 4 bytes in the union).
    // command is set by this function, not read
    unsigned_integral_and_array_read_from_socket<24>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                     status);
    header.command = USBIP_RET_UNLINK;
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink
usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(std::uint32_t seqnum, std::uint32_t status) {
    return {.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum), .status = status};
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink
usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(std::uint32_t seqnum) {
    return {.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK)};
}

array_data_type<calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST),
                                                decltype(UsbIpCommand::OpReqDevlist::status)>()>
usbipdcpp::UsbIpCommand::OpReqDevlist::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_DEVLIST, status);
}

void UsbIpCommand::OpReqDevlist::from_socket(asio::ip::tcp::socket &sock) {
    // status is a reserved field in client requests (specified as 0 by the protocol), read and ignored by the server:
    // do not assert status == 0; a malicious client can send any value, and a failed assertion in a debug build
    // would crash the server (a remotely triggerable crash point)
    status = read_u32(sock);
}

array_data_type<calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_IMPORT),
                                                decltype(UsbIpCommand::OpReqImport::status),
                                                decltype(UsbIpCommand::OpReqImport::busid)>()>
usbipdcpp::UsbIpCommand::OpReqImport::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_IMPORT, status, busid);
}

void UsbIpCommand::OpReqImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    data_read_from_socket(sock, status, busid);
    // As in OpReqDevlist: do not assert status == 0; status is a reserved request field,
    // and a malicious client can send any value, causing a failed assertion in a debug build to crash the server
}

void UsbIpCommand::UsbIpCmdSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock,
                asio::buffer(to_network_array(header.to_bytes(), transfer_flags, transfer_buffer_length, start_frame,
                                              number_of_packets, interval, setup.to_bytes())),
                ec);
    if (ec)
        return;
    if (transfer) {
        auto *op = transfer.get_operator();
        void *raw_handle = transfer.get();
        // The data stage is carried only in the OUT direction (symmetric with the direction check in from_socket):
        // IN always passes 0 to prevent accidentally sending the IN transfer buffer's contents
        op->send_transfer_data(raw_handle, sock, op->transfer_is_in(raw_handle) ? 0 : transfer_buffer_length, ec);
    }
}

void UsbIpCommand::UsbIpCmdSubmit::from_socket(asio::ip::tcp::socket &sock) {
    // Use scatter-gather to read the fixed portion in one operation
    // header fields (16 bytes) + transfer parameters (20 bytes) + setup (8 bytes) = 44 bytes
    decltype(SetupPacket{}.to_bytes()) setup_buffer;
    unsigned_integral_and_array_read_from_socket(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                 transfer_flags, transfer_buffer_length, start_frame, number_of_packets,
                                                 interval, setup_buffer);
    // Set the command type
    header.command = USBIP_CMD_SUBMIT;

    // Validate the endpoint number: header.ep is 32-bit, and conversion to uint8_t truncates it; a malicious client could send
    // an overflowing value (e.g. 0x100) that maps to the wrong endpoint after truncation. In the protocol, ep is an endpoint number without
    // a direction bit (direction is given by the direction field; see the comment for UsbIpHeaderBasic::ep
    // in protocol.h); its valid range is 0-0x7F, and out-of-range values are rejected as protocol errors
    if (header.ep > 0x7F) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::protocol_error), "invalid endpoint number");
    }

    // Parse the setup packet (little-endian)
    setup = SetupPacket::parse(setup_buffer);

    // Check the buffer size to prevent maliciously large memory allocations
    if (transfer_buffer_length > USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "transfer_buffer_length too large");
    }

    // Check the isochronous packet count to prevent maliciously huge allocations (e.g. 0x7FFFFFFF causes libusb_alloc_transfer
    // to fail allocation and return null). 0xFFFFFFFF is the protocol's placeholder for non-isochronous transfers and must be allowed
    if (number_of_packets != 0xFFFFFFFF && number_of_packets > USBIPDCPP_MAX_ISO_PACKETS) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "number_of_packets too large");
    }

    // Get the leaf op for the corresponding endpoint from the routing op, and use it to create transfer_handle
    // header.ep is in USB/IP wire format (without the direction bit); restore the actual endpoint address before looking it up
    int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;
    auto *routing_op = transfer.get_operator();
    std::uint8_t real_ep = static_cast<std::uint8_t>(header.ep);
    // direction is not explicitly validated (values other than 0/1 are treated as Out): this matches the kernel usbip module and
    // usbipd-libusb (both test only == USBIP_DIR_IN). Endpoint addresses reconstructed from invalid values
    // will not match any real endpoint; the subsequent find_ep failure naturally rejects them, with no memory
    // safety impact
    if (header.direction == UsbIpDirection::In)
        real_ep |= 0x80;
    auto *leaf_op = routing_op->get_operator_for_ep(real_ep);
    auto *raw_handle = leaf_op->alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup);
    if (!raw_handle) [[unlikely]] {
        // On allocation failure (out of memory, etc.), throw std::system_error, caught by get_cmd_from_socket to
        // disconnect this session gracefully rather than crashing the entire process through a null-pointer dereference
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "alloc_transfer_handle failed");
    }
    // Bind the handle to the leaf op so subsequent I/O operations use it directly without a map lookup
    transfer.set_handle(raw_handle, leaf_op);

    // All data transfer is handled by recv_transfer_data (data + iso descriptors)
    // For IN, the client sends no data; pass length 0
    std::error_code ec;
    leaf_op->recv_transfer_data(raw_handle, sock, header.direction == UsbIpDirection::In ? 0 : transfer_buffer_length,
                                ec);
    if (ec)
        throw std::system_error(ec);
}

array_data_type<calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()),
                                                decltype(UsbIpCommand::UsbIpCmdUnlink::unlink_seqnum)>() +
                24>
usbipdcpp::UsbIpCommand::UsbIpCmdUnlink::to_bytes() const {
    return array_add_padding<24>(to_network_array(header.to_bytes(), unlink_seqnum));
}

void UsbIpCommand::UsbIpCmdUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void UsbIpCommand::UsbIpCmdUnlink::from_socket(asio::ip::tcp::socket &sock) {
    // Use scatter-gather to read the header fields + unlink_seqnum + padding in one operation
    unsigned_integral_and_array_read_from_socket<24>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                     unlink_seqnum);
    // Set the command type
    header.command = USBIP_CMD_UNLINK;
}

usbipdcpp::UsbIpCommand::OpCmdVariant usbipdcpp::UsbIpCommand::get_op_from_socket(asio::ip::tcp::socket &sock,
                                                                                  usbipdcpp::error_code &ec) {
    try {
        auto version = read_u16(sock);
        if (version != 0 && version != USBIP_VERSION) {
            ec = make_error_code(ErrorType::UNKNOWN_VERSION);
            return OpCmdVariant{};
        }
        auto op = read_u16(sock);
        SPDLOG_DEBUG("Received op: 0x{:04x}", op);

        switch (op) {
            case OP_REQ_DEVLIST: {
                auto req = OpReqDevlist{};
                req.from_socket(sock);
                return req;
                break;
            }
            case OP_REQ_IMPORT: {
                auto req = OpReqImport{};
                req.from_socket(sock);
                return req;
                break;
            }
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                return UsbIpCommand::OpCmdVariant{};
            }
        }
    } catch (const asio::system_error &e) {
        SPDLOG_DEBUG("asio error: {}", e.what());
        if (e.code() == asio::error::eof) {
            ec = make_error_code(ErrorType::SOCKET_EOF);
        }
        else {
            ec = make_error_code(ErrorType::SOCKET_ERR);
        }
    }
    return UsbIpCommand::OpCmdVariant{};
}

usbipdcpp::UsbIpCommand::CmdVariant usbipdcpp::UsbIpCommand::get_cmd_from_socket(asio::ip::tcp::socket &sock,
                                                                                 AbstDeviceHandler *handler,
                                                                                 usbipdcpp::error_code &ec) {

    try {
        auto command = read_u32(sock);
        SPDLOG_DEBUG("Received command: 0x{:04x}", command);

        switch (command) {
            case USBIP_CMD_SUBMIT: {
                auto cmd = UsbIpCmdSubmit{};
                // Set operator in advance to prevent a null pointer
                cmd.transfer.set_operator(handler->get_transfer_operator());
                cmd.from_socket(sock);
                return cmd;
                break;
            }
            case USBIP_CMD_UNLINK: {
                auto cmd = UsbIpCmdUnlink{};
                cmd.from_socket(sock);
                return cmd;
                break;
            }
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                return UsbIpCommand::CmdVariant{};
            }
        }
    } catch (const asio::system_error &e) {
        // Note: in standalone asio, asio::system_error is a typedef for std::system_error,
        // so plain std::system_error exceptions thrown in from_socket (e.g. transfer_buffer_length exceeding its limit,
        // or failure to read the OUT data stage) are caught here too, instead of escaping the thread function and causing std::terminate
        SPDLOG_DEBUG("asio error: {}", e.what());
        if (e.code() == asio::error::eof) {
            ec = make_error_code(ErrorType::SOCKET_EOF);
        }
        else {
            ec = make_error_code(ErrorType::SOCKET_ERR);
        }
    }
    return UsbIpCommand::CmdVariant{};
}
