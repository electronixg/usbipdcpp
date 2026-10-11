#pragma once

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/utils/ObjectPool.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"

namespace usbipdcpp {

class MscBulkOnlyHandler;

/**
 * @brief MSC zero-copy transfer operator
 *
 * IN: send_transfer_data sends directly from external_buf (mmap)
 * OUT: recv_transfer_data reads directly into CBW/staging and parses
 */
class StorageTransferOperator : public TransferOperator {
public:
    explicit StorageTransferOperator(MscBulkOnlyHandler *handler);

    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    std::size_t get_actual_length(void *handle) override;

    bool transfer_is_in(void *handle) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;

private:
    MscBulkOnlyHandler *handler_;
    /// BOT has at most 2-3 transfers in flight; 8 slots suffice
    ObjectPool<StorageIoTransfer, 8> pool_;
};

} // namespace usbipdcpp
