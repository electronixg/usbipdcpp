#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"

#include <cstring>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/virtual_device/RndisConstants.h"

namespace usbipdcpp {

namespace {
// memcpy the content at the byte-stream offset into a packed struct: incoming RNDIS messages are byte sequences directly in
// the buffer; accessing packed members directly through reinterpret_cast triggers UBSan
// misaligned reports. memcpy into an aligned local object before field access is safe
template <typename T>
bool copy_msg(const data_type &data, std::size_t offset, T &out) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

// Serialize a u32 value as 4 little-endian bytes (for OID query return data)
data_type le32_bytes(std::uint32_t v) {
    data_type d;
    vector_append_to_le(d, v);
    return d;
}

// Assemble an INDICATE status message (MEDIA_CONNECT / MEDIA_DISCONNECT, 20 bytes)
data_type make_indicate(RndisStatus status) {
    data_type resp;
    RndisIndicateStatusMsg{rndis_msg(RndisMessageType::Indicate), 20, rndis_status(status), 0, 0}.append_to(resp);
    return resp;
}

// Wrap with a RNDIS_MSG_PACKET header (44B): MessageType=1, MessageLength=44+size,
// DataOffset=36 (data starts at byte 44), DataLength=size, others 0.
// If total length is a multiple of maxpacket(64), pad 1 byte to prevent ZLP (matching kernel u_ether.c: RNDIS
// disallows ZLP; MessageLength excludes padding, and the host trims the trailing byte after splitting by message length)
data_type wrap_rndis_packet(const std::uint8_t *data, std::size_t size) {
    data_type wrapped;
    wrapped.reserve(44 + size + 1);
    RndisPacketHeader{1, static_cast<std::uint32_t>(44 + size), 36, static_cast<std::uint32_t>(size),
                      0, 0, 0, 0, 0, 0, 0}
            .append_to(wrapped);
    wrapped.insert(wrapped.end(), data, data + size);
    if ((44 + size) % 64 == 0) {
        wrapped.push_back(0);
    }
    return wrapped;
}

// Parse the host's RNDIS_MSG_PACKET message stream, stripping headers to obtain the Ethernet frame list.
// One bulk OUT transfer may aggregate multiple messages (each starts immediately after the previous message's message_length);
// each message may have padding and nonzero OOB fields. Locate boundaries by message length,
// extract frames by DataOffset/DataLength, and ignore OOB directly (matching kernel rndis_rm_hdr
// and rndis_host.c rndis_rx_fixup). Discard frame lengths < 14 bytes (Ethernet header)
std::vector<data_type> unwrap_rndis_packets(const data_type &data) {
    std::vector<data_type> frames;
    std::size_t pos = 0;
    // Host-direction header is 24 bytes (RNDIS_HOST_HEADER_LEN); if insufficient, this and all following content are fragments
    while (pos + RNDIS_HOST_HEADER_LEN <= data.size()) {
        // Read only header fields through pointers (matching kernel rndis_rm_hdr's skb->data conversion):
        // the host sends only the first 24 bytes, so memcpy of the entire 44-byte struct goes out of bounds (observed with ASan)
        const auto *ph = reinterpret_cast<const RndisPacketHeader *>(data.data() + pos);
        // Invalid message length or exceeding transfer boundaries: discard this and all following messages
        if (ph->message_length < RNDIS_HOST_HEADER_LEN || pos + ph->message_length > data.size()) {
            break;
        }
        if (ph->message_type != rndis_msg(RndisMessageType::Packet)) {
            break; // Non-packet message on the data plane: abnormal host; discard
        }
        std::size_t frame_off = pos + data_start_offset(*ph); // Data start (see the header for the within-message offset calculation)
        std::size_t frame_len = ph->data_length;
        if (frame_off + frame_len > data.size()) {
            break;
        }
        if (frame_len >= 14) { // Minimum Ethernet header length (matching kernel u_ether's ETH_HLEN validation)
            frames.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(frame_off),
                                data.begin() + static_cast<std::ptrdiff_t>(frame_off + frame_len));
        }
        pos += ph->message_length;
    }
    return frames;
}
} // namespace

// ==================== RndisCommunicationInterfaceHandler ====================

RndisCommunicationInterfaceHandler::RndisCommunicationInterfaceHandler(
        UsbInterface &handle_interface, StringPool &string_pool, std::array<std::uint8_t, 6> mac_address,
        UsbSpeed speed) :
    VirtualInterfaceHandler(handle_interface, string_pool),
    mac_address_(mac_address), interface_number_(handle_interface.interface_number) {
    // Report LINK_SPEED (units of 100bps, matching kernel f_rndis.c bitrate()/100)
    switch (speed) {
        case UsbSpeed::Full:
            link_speed_100bps_ = 97'280; // 19×64×1×1000×8 bps
            break;
        case UsbSpeed::High:
            link_speed_100bps_ = 4'259'840; // 13×512×8×1000×8 bps
            break;
        case UsbSpeed::Super:
            link_speed_100bps_ = 37'500'000; // 3.75 Gbps
            break;
        case UsbSpeed::SuperPlus:
            link_speed_100bps_ = 42'500'000; // 4.25 Gbps
            break;
        default:
            link_speed_100bps_ = 0; // Low/Wireless/Unknown: do not report speed
            break;
    }
    // The notification channel's pending buffer is capped at 30 entries: one RESPONSE_AVAILABLE notification is issued when a response is
    // queued (single-digit count during handshake); 30 guards against memory accumulation if the host does not read for a long time
    notification_channel.set_max_pending_messages(30);
}

UsbInterface RndisCommunicationInterfaceHandler::make_interface(std::uint8_t interrupt_in_ep) {
    // Match kernel f_rndis.c's control interface: COMM class + ACM subclass + Vendor protocol.
    // Do not use Microsoft's proprietary RNDIS descriptors; Windows and Linux rndis_host both match
    // 02/02/FF (rndis_host.c's id_table)
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
            .interface_subclass = 0x02, // ACM (RNDIS uses the ACM wrapper for matching)
            .interface_protocol = 0xFF, // Vendor
            .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                       .max_packet_size = 8, // Status endpoint minimum 8 bytes (mandatory for cdc_ether binding)
                                       .interval = 32}}}, // Match kernel fs_control_intf_ep (Full speed)
    };
    // IAD: RNDIS is a two-interface function (communication+data); Windows uses it to recognize a composite
    // device and probe MS OS descriptors (matching kernel f_rndis.c's rndis_iad_descriptor:
    // bFunctionClass=CDC,bFunctionSubClass=ETHERNET(0x06),bInterfaceCount=2).
    // bFirstInterface is filled from its owning interface's interface_number when generating the configuration descriptor
    i.interface_association_descriptor = IadDesc::make(
            2, static_cast<std::uint8_t>(ClassCode::CDC), 0x06 /* USB_CDC_SUBCLASS_ETHERNET */);
    return i;
}

void RndisCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        // Non-class request (standard requests were already dispatched by the base class; this is the fallback)
        SPDLOG_WARN("RNDIS communication interface received non-class control request 0x{:x}", setup_packet.calc_request_type());
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto request = static_cast<CdcAcmRequest>(setup_packet.request);
    if (request == CdcAcmRequest::SendEncapsulatedCommand && setup_packet.is_out()) {
        // The host supplies one RNDIS command (data stage = entire message); parse and queue a response;
        // the host retrieves the response later through GET_ENCAPSULATED_RESPONSE (matching kernel f_rndis.c's
        // rndis_setup: parse the command in completion after receiving it fully through ep0 OUT)
        auto *trx = GenericTransfer::from_handle(transfer.get());
        process_rndis_message(trx->data);
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0));
    }
    else if (request == CdcAcmRequest::GetEncapsulatedResponse && !setup_packet.is_out()) {
        // The host retrieves one queued response. Return EPIPE for an empty queue (matching kernel rndis_setup's
        // STALL; host rndis_command polls and retries at 40ms intervals)
        auto *trx = GenericTransfer::from_handle(transfer.get());
        if (response_queue_.empty()) {
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
        else {
            trx->data = std::move(response_queue_.front());
            response_queue_.pop_front();
            // Truncate responses exceeding the host buffer to its size (host wLength is usually >= 1558, so this normally does not occur)
            if (trx->data.size() > transfer_buffer_length) {
                trx->data.resize(transfer_buffer_length);
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(trx->data.size()), std::move(transfer)));
        }
    }
    else {
        SPDLOG_WARN("RNDIS communication interface received unexpected control request 0x{:x} dir={}", setup_packet.request,
                    setup_packet.is_out() ? "OUT" : "IN");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void RndisCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                   std::uint32_t transfer_flags,
                                                                   std::uint32_t transfer_buffer_length,
                                                                   TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Handled within the channel: reply immediately if a notification is buffered; otherwise suspend until response enqueue pushes one
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Interrupt OUT: unused by RNDIS (matching ECM handling)
        SPDLOG_WARN("RNDIS communication interface received unexpected interrupt OUT");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type RndisCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // Header + CallManagement + ACM + Union (matching kernel f_rndis.c's
    // rndis_control_intf descriptor combination; reuse CdcAcmConstants for common CDC structures)
    data_type descriptor;

    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    // bDataInterface is the data interface number: associated by set_data_handler during assembly, obtained from the data
    // handler's get_interface() (a composite device's communication interface does not start at interface 0; hardcoding
    // 1 prevents the host driver finding the data interface). Fall back to 1 when unassociated (single-device form COMM=0 -> DATA=1)
    CdcCallManagementDesc{0x05, CS_INTERFACE, 0x01,
                          0x00, // bmCapabilities: the data interface does not handle call management
                          data_handler_ ? static_cast<std::uint8_t>(data_handler_->get_interface().interface_number) : std::uint8_t{1}}
            .append_to(descriptor);

    CdcAcmFunctionalDesc{0x04, CS_INTERFACE, 0x02,
                         0x00} // bmCapabilities must be 0: the cdc_ether host driver
            // treats nonzero as not genuine RNDIS and rejects binding (cdc_ether.c:225-235)
            .append_to(descriptor);

    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           get_interface().interface_number, // bMasterInterface: this interface
                           data_handler_ ? static_cast<std::uint8_t>(data_handler_->get_interface().interface_number) : std::uint8_t{1}}
            .append_to(descriptor);

    return descriptor;
}

void RndisCommunicationInterfaceHandler::process_rndis_message(const data_type &msg) {
    RndisMessageHeader hdr{};
    if (!copy_msg(msg, 0, hdr)) {
        SPDLOG_WARN("RNDIS message too short: {} bytes", msg.size());
        return;
    }
    if (hdr.message_length < sizeof(RndisMessageHeader) || hdr.message_length > msg.size()) {
        SPDLOG_WARN("Invalid RNDIS message length: declared {} received {}", hdr.message_length, msg.size());
        return;
    }

    switch (static_cast<RndisMessageType>(hdr.message_type)) {
        case RndisMessageType::Init: {
            RndisInitMsg init{};
            if (!copy_msg(msg, 0, init)) {
                return;
            }
            state_ = RndisState::Initialized;
            data_type resp;
            RndisInitCmplt{rndis_msg(RndisMessageType::InitComplete), 52, init.request_id,
                           rndis_status(RndisStatus::Success), RNDIS_MAJOR_VERSION, RNDIS_MINOR_VERSION,
                           RNDIS_DF_CONNECTIONLESS, RNDIS_MEDIUM_802_3, 1 /* MaxPacketsPerTransfer */,
                           RNDIS_MAX_TRANSFER_SIZE, 0 /* PacketAlignmentFactor */, 0 /* AFListOffset */,
                           0 /* AFListSize */}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            // Link-up notification: match kernel rndis_open's rndis_signal_connect.
            // The link is available once virtual device initialization completes (Windows relies on this notification to confirm connection)
            enqueue_response(make_indicate(RndisStatus::MediaConnect));
            break;
        }
        case RndisMessageType::Halt:
            // Do not respond to HALT (matching rndis.c: farewell message on host unbinding/disconnection; reset state)
            reset_device_state();
            break;
        case RndisMessageType::Query: {
            RndisQueryMsg query{};
            if (!copy_msg(msg, 0, query)) {
                return;
            }
            data_type payload = query_oid(query.oid);
            // For unknown/unsupported OIDs return QUERY_C with NOT_SUPPORTED (do not stall the control request)
            auto status = payload.empty() ? RndisStatus::NotSupported : RndisStatus::Success;
            data_type resp;
            RndisQueryCmplt{rndis_msg(RndisMessageType::QueryComplete),
                            static_cast<std::uint32_t>(24 + payload.size()), query.request_id,
                            rndis_status(status), static_cast<std::uint32_t>(payload.size()),
                            16 /* InformationBufferOffset: data immediately follows the 24-byte header */}
                    .append_to(resp);
            resp.insert(resp.end(), payload.begin(), payload.end());
            enqueue_response(std::move(resp));
            break;
        }
        case RndisMessageType::Set: {
            RndisSetMsg set{};
            if (!copy_msg(msg, 0, set)) {
                return;
            }
            // Input buffer start = message byte 8 + offset (matching kernel rndis.c's offset convention)
            std::size_t buf_off = 8 + set.information_buffer_offset;
            RndisStatus status = RndisStatus::NotSupported;
            switch (set.oid) {
                case OID_GEN_CURRENT_PACKET_FILTER: {
                    if (buf_off + 4 > msg.size()) {
                        status = RndisStatus::InvalidData;
                        break;
                    }
                    std::uint32_t filter_le{};
                    std::memcpy(&filter_le, msg.data() + buf_off, 4);
                    packet_filter_ = static_cast<std::uint16_t>(filter_le); // Low 16 bits are the filter bitmap
                    // Match rndis.c: filter!=0 -> DATA_INITIALIZED (carrier on, begin
                    // sending/receiving); filter==0 -> return to INITIALIZED (carrier off)
                    state_ = packet_filter_ != 0 ? RndisState::DataInitialized : RndisState::Initialized;
                    SPDLOG_DEBUG("RNDIS SET_CURRENT_PACKET_FILTER: 0x{:04x}", packet_filter_);
                    status = RndisStatus::Success;
                    break;
                }
                case OID_802_3_MULTICAST_LIST:
                    // Accept directly (ignore contents, matching kernel gen_ndis_set_resp's default acceptance)
                    SPDLOG_DEBUG("RNDIS SET_802_3_MULTICAST_LIST accepted");
                    status = RndisStatus::Success;
                    break;
                default:
                    // Other OIDs unsupported (matching kernel SET returning NOT_SUPPORTED for unknown OIDs)
                    SPDLOG_WARN("RNDIS SET OID 0x{:08X} unsupported", set.oid);
                    break;
            }
            data_type resp;
            RndisSetCmplt{rndis_msg(RndisMessageType::SetComplete), 16, set.request_id, rndis_status(status)}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            break;
        }
        case RndisMessageType::Reset:
            // Match rndis.c: clear unretrieved response queue and return RESET_C (AddressingReset=1)
            response_queue_.clear();
            {
                data_type resp;
                RndisResetCmplt{rndis_msg(RndisMessageType::ResetComplete), 16, rndis_status(RndisStatus::Success),
                                1 /* AddressingReset */}
                        .append_to(resp);
                enqueue_response(std::move(resp));
            }
            break;
        case RndisMessageType::Keepalive: {
            // KEEPALIVE has only a 12-byte header with request_id in it (matching rndis.c:
            // host (Windows) sends every 5 seconds; return KEEPALIVE_C SUCCESS)
            RndisMessageHeader ka{};
            if (!copy_msg(msg, 0, ka)) {
                return;
            }
            data_type resp;
            RndisKeepaliveCmplt{rndis_msg(RndisMessageType::KeepaliveComplete), 16, ka.request_id,
                                rndis_status(RndisStatus::Success)}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            break;
        }
        default:
            // Unknown message type: discard without response (matching rndis.c's default branch)
            SPDLOG_WARN("Unknown RNDIS message type 0x{:08X}", hdr.message_type);
            break;
    }
}

data_type RndisCommunicationInterfaceHandler::query_oid(std::uint32_t oid) {
    // The OID table matches kernel rndis.c's oid_supported_list and rndis_query_response;
    // returning empty means unsupported (assemble QUERY_C with NOT_SUPPORTED)
    switch (oid) {
        case OID_GEN_SUPPORTED_LIST: {
            // Supported list = all except statistics OIDs (XMIT_OK/RCV_OK/...), matching the kernel
            const std::uint32_t list[] = {
                    OID_GEN_SUPPORTED_LIST,       OID_GEN_HARDWARE_STATUS,  OID_GEN_MEDIA_SUPPORTED,
                    OID_GEN_MEDIA_IN_USE,         OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED,
                    OID_GEN_TRANSMIT_BLOCK_SIZE,  OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
                    OID_GEN_VENDOR_DESCRIPTION,   OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
                    OID_GEN_MAXIMUM_TOTAL_SIZE,   OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAC_OPTIONS,
                    OID_GEN_PHYSICAL_MEDIUM,      OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
                    OID_802_3_MULTICAST_LIST,     OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_MAC_OPTIONS,
            };
            data_type buf;
            for (auto supported : list) {
                vector_append_to_le(buf, supported); // 4 bytes per entry
            }
            return buf;
        }
        case OID_GEN_HARDWARE_STATUS:
            return le32_bytes(0); // Hardware ready
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            return le32_bytes(RNDIS_MEDIUM_802_3);
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            return le32_bytes(1500); // netdev mtu (matching the kernel)
        case OID_GEN_LINK_SPEED:
            return le32_bytes(link_speed_100bps_);
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            return le32_bytes(1500);
        case OID_GEN_VENDOR_ID:
            return le32_bytes(0); // Low 24 bits of vendor OUI (unconfigured in the example)
        case OID_GEN_VENDOR_DESCRIPTION:
            return le32_bytes(0); // No configured vendor description: match the kernel by returning an empty description
        case OID_GEN_VENDOR_DRIVER_VERSION:
            return le32_bytes(1);
        case OID_GEN_CURRENT_PACKET_FILTER:
            return le32_bytes(packet_filter_);
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            return le32_bytes(RNDIS_MAX_TOTAL_SIZE);
        case OID_GEN_MEDIA_CONNECT_STATUS:
            return le32_bytes(0); // CONNECTED: usbip connection means link up
        case OID_GEN_MAC_OPTIONS:
            return le32_bytes(RNDIS_MAC_OPTIONS_SERIALIZED_FULL_DUPLEX);
        case OID_GEN_PHYSICAL_MEDIUM:
            return le32_bytes(0); // UNSPECIFIED
        case OID_GEN_XMIT_OK:
        case OID_GEN_RCV_OK:
        case OID_GEN_XMIT_ERROR:
        case OID_GEN_RCV_ERROR:
        case OID_GEN_RCV_NO_BUFFER:
            return le32_bytes(0); // Statistics (not collected by the example)
        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS: {
            // 6-byte binary MAC (Linux host binding mandates querying PERMANENT_ADDRESS;
            // failure prevents binding: rndis_host.c generic_rndis_bind)
            return {mac_address_[0], mac_address_[1], mac_address_[2], mac_address_[3], mac_address_[4],
                    mac_address_[5]};
        }
        case OID_802_3_MULTICAST_LIST:
            return le32_bytes(0xE0000000); // Multicast base address
        case OID_802_3_MAXIMUM_LIST_SIZE:
            return le32_bytes(1);
        case OID_802_3_MAC_OPTIONS:
            return le32_bytes(0);
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            return le32_bytes(0);
        default:
            return {}; // Unknown OID -> NOT_SUPPORTED
    }
}

void RndisCommunicationInterfaceHandler::enqueue_response(data_type &&response) {
    if (response_queue_.size() >= RNDIS_RESPONSE_QUEUE_LIMIT) {
        response_queue_.pop_front(); // Prevent malicious host accumulation: drop the oldest
    }
    response_queue_.push_back(std::move(response));
    // Notify the host a response is available (matching kernel resp_avail callback: interrupt IN sends 8 bytes {1,0})
    notification_channel.push(make_rndis_response_available_notification());
}

void RndisCommunicationInterfaceHandler::reset_device_state() {
    state_ = RndisState::Uninitialized;
    packet_filter_ = 0;
    response_queue_.clear();
}

void RndisCommunicationInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state.
    // A new client starts a fresh RNDIS session: the state machine starts at UNINITIALIZED
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    reset_device_state();
    notification_channel.on_new_connection(&current_session);
}

void RndisCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically) and reset the state machine,
    // then let the parent clear session
    notification_channel.on_disconnection();
    reset_device_state();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void RndisCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== RndisDataInterfaceHandler ====================

RndisDataInterfaceHandler::RndisDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                     NetworkBackend *backend,
                                                     RndisCommunicationInterfaceHandler *comm) :
    VirtualInterfaceHandler(handle_interface, string_pool), backend_(backend), comm_(comm) {
    if (backend_) {
        // Inject the frame-send callback: wrap backend-produced frames with RNDIS headers, then push to the host through the channel
        // (the channel is thread-safe, callable from any thread)
        backend_->set_send_to_host([this](const std::uint8_t *data, std::size_t size) {
            in_channel.push(wrap_rndis_packet(data, size));
        });
    }
}

UsbInterface RndisDataInterfaceHandler::make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
    // Match kernel f_rndis.c's rndis_data_intf: only alt0 (no NOP altsetting;
    // this is RNDIS's advantage over ECM: RNDIS controls sending/receiving with an OID packet filter, without needing
    // altsetting switches for the data plane)
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {{UsbEndpoint{.address = in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0},
                           UsbEndpoint{.address = out_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0}}},
    };
    return i;
}

void RndisDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     std::uint32_t transfer_flags,
                                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                     std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN: the host requests a frame. Handled within the channel: reply immediately if a frame is buffered (one message =
        // one RNDIS_MSG_PACKET frame; send a short packet if shorter than the request), otherwise suspend and wait
        SPDLOG_TRACE("[RNDIS] IN req seq={} len={} buf={}", seqnum, transfer_buffer_length, in_channel.size());
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT: the host sends a RNDIS_MSG_PACKET message stream. If backend exists, parse and strip headers, then
        // give each frame to the network side for consumption and reply immediately. Otherwise let the subclass consume immediately (true=
        // handled); if neither works, suspend in the channel (NAK backpressure), and the subclass later takes
        // data with take_frame() and responds (the original message is suspended without stripping headers)
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        SPDLOG_TRACE("[RNDIS] OUT raw={} bytes", received_size);
        auto frames = unwrap_rndis_packets(trx->data);
        bool consumed = false;
        if (backend_) {
            for (auto &frame : frames) {
                backend_->send_frame(frame.data(), frame.size());
            }
            consumed = true;
        }
        else if (!frames.empty()) {
            consumed = true;
            for (auto &frame : frames) {
                if (!on_frame_received(std::move(frame))) {
                    consumed = false;
                    break;
                }
            }
        }
        if (consumed) {
            // Reply normally even if parsing fails (no valid frames): match the kernel dropping packets without stalling the URB
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void RndisDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // The data interface has no class-specific control requests (RNDIS control uses encapsulated commands on the communication interface)
    SPDLOG_WARN("RNDIS data interface received unexpected control request");
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type RndisDataInterfaceHandler::get_class_specific_descriptor() {
    // The data interface has no class-specific descriptors (matching kernel rndis_data_intf)
    return {};
}

bool RndisDataInterfaceHandler::on_frame_received(data_type &&frame) {
    // Default behavior: always consume and reply immediately (subclasses without overrides retain always-receive semantics)
    return true;
}

std::size_t RndisDataInterfaceHandler::send_frame(const std::uint8_t *data, std::size_t size) {
    // Nonblocking push (the channel discards and returns 0 when disconnected), automatically wraps RNDIS headers;
    // return value is the frame length (matching ECM send_frame semantics)
    if (data == nullptr || size == 0) {
        return 0;
    }
    SPDLOG_TRACE("[RNDIS] push frame size={} buf_before={}", size, in_channel.size());
    in_channel.push(wrap_rndis_packet(data, size));
    return size;
}

std::size_t RndisDataInterfaceHandler::send_frame(const data_type &frame) {
    return send_frame(frame.data(), frame.size());
}

std::optional<OutEndpointChannel::Pending> RndisDataInterfaceHandler::take_frame(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> RndisDataInterfaceHandler::try_take_frame() {
    return out_channel.try_take();
}

void RndisDataInterfaceHandler::set_tx_max_pending(std::size_t max_pending) {
    // The channel's internal lock protects the buffer limit
    in_channel.set_max_pending_messages(max_pending);
}

void RndisDataInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // The parent sets the session pointer first (needed for channel responses), then bind the channel and reset disconnection state
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void RndisDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // Clear the channel first (buffer + pending requests; TransferHandle destruction releases automatically; wake blocked
    // takers to return as disconnected), clear backend buffers to prevent old data remaining, then let the parent clear session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    if (backend_) {
        backend_->reset();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void RndisDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // Pending requests are distributed across the IN/OUT channels; try cancelling each in turn
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
} // namespace usbipdcpp
