#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "usbipdcpp/virtual_device/MscConstants.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"

namespace usbipdcpp {

class StorageTransferOperator;

/** Identification strings returned by SCSI INQUIRY / VPD.
 *  Empty strings are read automatically from VirtualDeviceHandler's USB descriptors. */
struct MscConfig {
    std::string vendor; // INQUIRY 8-byte vendor name
    std::string product; // INQUIRY 16-byte product name
    std::string revision; // INQUIRY 4-byte revision number
    std::string serial; // VPD 0x80 serial number
};

/** MSC Bulk-Only Transport protocol handler.
 *
 * BOT is a synchronous protocol (CBW→Data→CSW); all IN data is ready upon receiving CBW,
 * so host IN requests can be answered immediately, without EndpointRequestQueue (unlike HID/CDC ACM
 * and other devices producing data asynchronously, which need queues to hold IN requests until data is ready). */
class USBIPDCPP_API MscBulkOnlyHandler : public VirtualInterfaceHandler {
public:
    MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool, std::unique_ptr<StorageBackend> backend,
                       MscConfig config = {}, bool read_only = false);

    /**
     * @brief Create a USB Mass Storage bulk-transfer interface (descriptor template, no handler bound)
     *
     * Interface definition: Mass Storage class, SCSI transparent subclass, Bulk-Only protocol
     * (08/06/50), one Bulk IN + one Bulk OUT endpoint (High speed, mps=512).
     * @param in_ep Bulk IN endpoint address (device→host, command/data responses)
     * @param out_ep Bulk OUT endpoint address (host→device, CBW/data)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::MassStorage),
                .interface_subclass = 0x06, // SCSI transparent
                .interface_protocol = 0x50, // Bulk-Only Transport
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 512,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 512,
                                           .interval = 0}}},
        };
        return i;
    }

    /** Bulk-Only Transport core: send IN data (DataIn/Status), only ack OUT (data already handled by on_out_data_received) */
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /** Provide target buffers for OUT transfers (Idle→fallback / DataOut→mmap or staging) */
    void *prepare_out_buffer(std::size_t length, StorageIoTransfer *trx);
    /** Callback after receiving all OUT data, driving the BOT state machine: parse CBW or write to disk or UNMAP */
    void on_out_data_received(StorageIoTransfer *trx, std::size_t length);

    StorageBackend *get_backend() const {
        return backend_.get();
    }

    /** Callback after device_handler is set, filling empty MscConfig fields from USB strings */
    void on_setup_interface_handlers() override;
    /** Reset BOT state machine on client connection */
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    /** Reset BOT state machine on client disconnection */
    void on_disconnection(error_code &ec) override;

private:
    std::unique_ptr<StorageBackend> backend_;
    bool read_only_ = false;
    MscConfig config_; // Fill empty fields in on_setup_interface_handlers

    /** BOT state machine: Idle → DataIn/DataOut → Status → Idle */
    BotState state_ = BotState::Idle;
    /** Most recently received CBW; CSW must return dCBWTag unchanged */
    CBW current_cbw_{};

    /** IN/OUT data staging or zero-copy mmap pointer.
     *  Delay clearing until the next CBW (Idle branch), preventing clearing while the previous command's sender thread still reads */
    std::vector<std::uint8_t> staging_data_;
    std::size_t staging_offset_ = 0; // Bytes sent during IN transfers (shared by staging / mmap)

    /** Transfer residue: dCBWDataTransferLength - actual bytes transferred, required by CSW */
    std::uint32_t data_residue_ = 0;
    /** Set true during CBW parsing, clear after Status sends CSW */
    bool command_failed_ = false;

    /** WRITE command target LBA and block count (10-byte CDB, 32-bit LBA) */
    std::uint64_t write_lba_ = 0;
    std::uint16_t write_count_ = 0;
    /** When true, DataOut parses UNMAP descriptors instead of writing to disk with WRITE */
    bool data_out_unmap_ = false;

    /** WRITE SAME fill write: target LBA, block count (16-byte CDB supports up to 32-bit block counts) */
    std::uint64_t write_same_lba_ = 0;
    std::uint64_t write_same_count_ = 0;
    /** When true, DataOut uses WRITE SAME fill mode (receive 1 block and fill the whole range with it) */
    bool data_out_write_same_ = false;

    /** READ zero-copy: mmap base address, starting LBA, total bytes */
    std::uint64_t read_lba_ = 0;
    void *read_mmap_base_ = nullptr;
    std::size_t read_total_size_ = 0;
    /** WRITE zero-copy: mmap base address, bytes received */
    void *write_mmap_base_ = nullptr;
    std::size_t write_accumulated_ = 0;

    void send_stall(std::uint32_t seqnum);

public:
    // ========== Standard request default implementation ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        return {};
    }
};

} // namespace usbipdcpp
