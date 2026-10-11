// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "usbipdcpp/virtual_device/storage_backends/StorageTransferOperator.h"

#include <spdlog/spdlog.h>
#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"

using namespace usbipdcpp;

StorageTransferOperator::StorageTransferOperator(MscBulkOnlyHandler *handler) : handler_(handler) {
}

void *StorageTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int, const UsbIpHeaderBasic &header,
                                                     const SetupPacket &) {
    auto *trx = pool_.alloc();
    if (!trx)
        trx = new StorageIoTransfer{};
    // Direction-recording contract: transfer_is_in depends on it (protocol layer determines return length from direction)
    trx->is_in = (header.direction == UsbIpDirection::In);
    SPDLOG_DEBUG("STO::alloc handle={:p} dir={} len={}", static_cast<const void *>(trx),
                 header.direction == UsbIpDirection::In ? "IN" : "OUT", buffer_length);
    if (header.direction == UsbIpDirection::Out) {
        trx->external_buf = handler_->prepare_out_buffer(buffer_length, trx);
        SPDLOG_DEBUG("STO::alloc OUT external_buf={:p}", static_cast<const void *>(trx->external_buf));
    }
    return trx;
}

void StorageTransferOperator::free_transfer_handle(void *handle) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    SPDLOG_DEBUG("STO::free handle={:p}", static_cast<const void *>(handle));
    trx->reset();
    if (!pool_.free(trx))
        delete trx;
}

std::size_t StorageTransferOperator::get_actual_length(void *handle) {
    return StorageIoTransfer::from_handle(handle)->actual_length;
}

bool StorageTransferOperator::transfer_is_in(void *handle) {
    return StorageIoTransfer::from_handle(handle)->is_in;
}

UsbIpIsoPacketDescriptor StorageTransferOperator::get_iso_descriptor(void *, int) {
    // MSC has no isochronous transfers
    return {};
}

void StorageTransferOperator::set_iso_descriptor(void *, int, const UsbIpIsoPacketDescriptor &) {
}

void StorageTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    auto *backend = handler_->get_backend();

    SPDLOG_DEBUG("STO::send handle={:p} len={} direct_io={} lba={} offset={} ext_buf={:p}",
                 static_cast<const void *>(handle), length, trx->direct_io, trx->file_lba, trx->file_offset,
                 static_cast<const void *>(trx->external_buf));

    // Only mmap READ uses zero-copy sendfile/TransmitFile; CSW and other fallback data do not access the file
    if (trx->direct_io && backend &&
        backend->send_direct(trx->file_lba, trx->file_offset, length, static_cast<intptr_t>(sock.native_handle()),
                             ec)) {
        SPDLOG_DEBUG("STO::send direct OK");
        return;
    }
    ec.clear();

    // Fallback: send from external_buf / fallback_data
    void *buf = trx->external_buf ? trx->external_buf : trx->fallback_data.data();
    SPDLOG_DEBUG("STO::send fallback buf={:p}", static_cast<const void *>(buf));
    asio::write(sock, asio::buffer(static_cast<const char *>(buf), length), ec);
}

void StorageTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    auto *backend = handler_->get_backend();

    SPDLOG_DEBUG("STO::recv handle={:p} len={} direct_io={} lba={} offset={} ext_buf={:p}",
                 static_cast<const void *>(handle), length, trx->direct_io, trx->file_lba, trx->file_offset,
                 static_cast<const void *>(trx->external_buf));

    // Only mmap WRITE uses zero-copy splice; CBW/staging data do not access the file
    if (trx->direct_io && trx->external_buf && backend &&
        backend->recv_direct(trx->file_lba, trx->file_offset, length, static_cast<intptr_t>(sock.native_handle()),
                             ec)) {
        SPDLOG_DEBUG("STO::recv direct OK");
        handler_->on_out_data_received(trx, length);
        return;
    }
    ec.clear();

    // Fallback: read directly into external_buf or fallback_data
    void *buf = trx->external_buf;
    if (buf) {
        SPDLOG_DEBUG("STO::recv fallback ext_buf={:p}", static_cast<const void *>(buf));
        asio::read(sock, asio::buffer(static_cast<std::uint8_t *>(buf), length), ec);
    }
    else {
        SPDLOG_DEBUG("STO::recv fallback resize={}", length);
        trx->fallback_data.resize(length);
        asio::read(sock, asio::buffer(trx->fallback_data), ec);
    }
    if (!ec) {
        handler_->on_out_data_received(trx, length);
    }
}
