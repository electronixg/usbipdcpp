#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"

#include <cstdlib>

#include <asio.hpp>
#include <libusb.h>
#include <spdlog/spdlog.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/utils/SmallVector.h"

using namespace usbipdcpp;

namespace usbipdcpp::detail {

libusb_transfer *LibusbTransferLM::create() {
    return libusb_alloc_transfer(0);
}

void LibusbTransferLM::destroy(libusb_transfer *p) {
    libusb_free_transfer(p);
}

void LibusbTransferReset::reset(libusb_transfer &t) {
    t.actual_length = 0;
    t.status = LIBUSB_TRANSFER_COMPLETED;
    // num_iso_packets is set only by libusb_fill_iso_transfer, and the pool reclaims only non-isochronous
    // transfers (free_transfer_handle returns to the pool when num_iso_packets==0), so reused objects
    // should always have 0. Explicitly clearing it makes this invariant local, preventing a remaining value from being misread by a non-isochronous path
    // if the pool policy changes in the future (e.g. allowing isochronous transfers into the pool)
    t.num_iso_packets = 0;
}

} // namespace usbipdcpp::detail

void *LibusbTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                    const UsbIpHeaderBasic &header, const SetupPacket &setup_packet) {
    libusb_transfer *trx;
    if (num_iso_packets == 0) {
        trx = transfer_pool_.alloc();
        if (!trx) [[unlikely]] {
            trx = libusb_alloc_transfer(0);
            if (!trx) [[unlikely]] {
                return nullptr;
            }
        }
    }
    else {
        trx = libusb_alloc_transfer(num_iso_packets);
        if (!trx) [[unlikely]] {
            return nullptr;
        }
        // libusb_alloc_transfer does not set the public num_iso_packets field (explicitly documented in io.c L443);
        // the user must assign it, or the descriptor read loop in recv_transfer_data reads garbage and misaligns the protocol.
        trx->num_iso_packets = num_iso_packets;
    }

    std::size_t write_offset = (header.ep == 0) ? LIBUSB_CONTROL_SETUP_SIZE : 0;
    std::size_t actual_buffer_length = buffer_length + write_offset;

    trx->buffer = static_cast<unsigned char *>(malloc(actual_buffer_length));
    if (!trx->buffer) [[unlikely]] {
        if (num_iso_packets == 0) {
            if (!transfer_pool_.free(trx))
                libusb_free_transfer(trx);
        }
        else {
            libusb_free_transfer(trx);
        }
        return nullptr;
    }
    trx->length = static_cast<int>(actual_buffer_length);
    return trx;
}

void LibusbTransferOperator::free_transfer_handle(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    free(trx->buffer);
    if (trx->num_iso_packets == 0) {
        if (!transfer_pool_.free(trx))
            libusb_free_transfer(trx);
    }
    else {
        libusb_free_transfer(trx);
    }
}

std::size_t LibusbTransferOperator::get_actual_length(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    return trx->actual_length;
}

bool LibusbTransferOperator::transfer_is_in(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    // Direction follows the endpoint used for libusb submission (the same basis as send_transfer_data's internal check)
    return (trx->endpoint & LIBUSB_ENDPOINT_IN) != 0;
}

UsbIpIsoPacketDescriptor LibusbTransferOperator::get_iso_descriptor(void *handle, int index) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    // libusb iso packets occupy consecutive buffer positions; offset = sum of all preceding packet lengths
    unsigned offset = 0;
    for (int i = 0; i < index; i++) {
        offset += trx->iso_packet_desc[i].length;
    }
    return UsbIpIsoPacketDescriptor{
            .offset = offset,
            .length = iso.length,
            .actual_length = iso.actual_length,
            .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(iso.status)),
    };
}

void LibusbTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    iso.status = LibusbDeviceHandler::error2trxstat(desc.status);
    iso.actual_length = desc.actual_length;
    iso.length = desc.length;
}

void LibusbTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && trx->num_iso_packets > 0) {
        SmallVector<asio::const_buffer, 130> buffers;
        SmallVector<decltype(UsbIpIsoPacketDescriptor{}.to_bytes()), 130> desc_bytes;
        // offset: packet-slot offset in buffer (pkt.length stride), used both for data reads and the descriptor offset field.
        //   Slot size is determined by descriptor length in the client's CMD_SUBMIT; advance by pkt.length rather than actual_length,
        //   otherwise vhci incorrectly writes packet N's data into packet N-1's slot.
        // length was already calculated by the protocol layer according to direction (transfer_is_in query; OUT always passes 0;
        // see RetSubmit/CmdSubmit to_socket). Here only length > 0 determines
        // whether to send data; do not determine direction independently again
        bool need_to_send_buffer = (length > 0);
        std::uint32_t offset = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            auto &pkt = trx->iso_packet_desc[i];
            if (need_to_send_buffer)
                buffers.push_back(asio::buffer(trx->buffer + offset, pkt.actual_length));
            UsbIpIsoPacketDescriptor desc{
                    .offset = offset,
                    .length = pkt.length,
                    .actual_length = pkt.actual_length,
                    .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(pkt.status)),
            };
            desc_bytes.push_back(desc.to_bytes());
            offset += pkt.length;
        }
        for (auto &bytes: desc_bytes) {
            buffers.push_back(asio::buffer(bytes));
        }
        asio::write(sock, buffers, ec);
    }
    else if (length > 0) {
        auto *buf = trx->buffer + (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        asio::write(sock, asio::buffer(buf, length), ec);
    }
}

void LibusbTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (length > 0) {
        // The first 8 bytes of the control-transfer buffer are reserved for the setup packet, filled later by receive_urb;
        // read data-stage content starting at offset 8 here.
        // trx->type is not set yet, so it cannot determine the transfer type;
        // use trx->length instead: allocation adds LIBUSB_CONTROL_SETUP_SIZE for control transfers,
        // so trx->length > length indicates the buffer includes the setup prefix
        bool is_control = (static_cast<std::size_t>(trx->length) > length);
        auto *buf = trx->buffer + (is_control ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        asio::read(sock, asio::buffer(buf, length), ec);
        if (ec)
            return;
    }

    // Validate and read ISO descriptors: length/actual_length are client-controlled fields and must be checked
    // before writing them into a libusb transfer. libusb reads/writes buffer data according to length; a sum of lengths
    // exceeding the buffer size (trx->length; ISO transfers have no setup prefix) causes out-of-bounds reads/writes
    // (heap overflow), while actual_length exceeding length overflows packet data. Reject the entire invalid
    // command (the caller throws and disconnects when ec is nonempty)
    std::uint64_t total_length = 0;
    for (int i = 0; i < trx->num_iso_packets; i++) {
        UsbIpIsoPacketDescriptor iso_desc{};
        iso_desc.from_socket(sock);
        // Write the check as total_length + length > trx->length: with
        // length > trx->length - total_length, if total_length exceeds
        // trx->length, unsigned subtraction underflows to a huge value and invalidates the check (preceding checks
        // ensure total_length is always <= trx->length, so this cannot actually occur, but readability is poor)
        if (iso_desc.actual_length > iso_desc.length ||
            total_length + iso_desc.length > static_cast<std::uint64_t>(trx->length)) [[unlikely]] {
            SPDLOG_ERROR("Invalid ISO descriptor: packet {} length={} actual_length={} (remaining buffer {})",
                         i, iso_desc.length, iso_desc.actual_length,
                         static_cast<std::uint64_t>(trx->length) - total_length);
            ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }
        total_length += iso_desc.length;
        // The client descriptor's offset is discarded (set_iso_descriptor does not write it): libusb's
        // iso_packet_desc has no offset field (buffer layout is implicitly contiguous), and the sender
        // calculates offset itself by accumulating pkt.length in send_transfer_data, so malicious or
        // incorrect offsets cannot affect server data positioning. Protocol wire-format data is inherently
        // compact: the client (Linux vhci), when sending ISO data, compactly copies from its local
        // URB buffer into the PDU according to descriptor offsets (kernel usbip_common.c's
        // usbip_alloc_iso_desc_pdu: memcpy destinations advance contiguously by each length),
        // so there is no on-wire layout with gaps
        set_iso_descriptor(handle, i, iso_desc);
    }
}
