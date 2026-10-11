#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "usbipdcpp/protocol.h"

namespace usbipdcpp {

/**
 * @brief Transfer dedicated to MSC storage I/O, used with StorageTransferOperator
 *
 * Does not allocate internal GenericTransfer::data; all I/O reaches target addresses directly through this struct's members.
 * Supports two data paths: zero-copy (mmap file memory) and fallback (staging_data_ / fallback_data).
 *
 * === IN direction (device → host, READ / CSW) ===
 * handle_bulk_transfer sets external_buf and actual_length; the sender thread sends through
 * send_transfer_data. Prefer sendfile / TransmitFile (direct_io), fall back to asio::write.
 * - READ mmap: external_buf points to file memory mapped by RawImageBackend, direct_io = true
 * - READ fallback: external_buf points into staging_data_
 * - CSW: external_buf points to this struct's fallback_data, direct_io = false
 *
 * === OUT direction (host → device, CBW / WRITE / UNMAP) ===
 * During alloc_transfer_handle, prepare_out_buffer sets external_buf;
 * recv_transfer_data prefers splice directly into the file (direct_io), falling back to asio::read into external_buf,
 * then on_out_data_received parses CBW or accumulates write data.
 * - CBW: external_buf is nullptr, uses fallback_data, direct_io = false
 * - WRITE mmap: external_buf points to mmap file memory, direct_io = true
 * - WRITE fallback / UNMAP: external_buf points to reserved space at the end of staging_data_
 */
struct StorageIoTransfer {
    // ===== Set by MscBulkOnlyHandler =====

    /**
     * @brief Target address for direct reads/writes
     *
     * IN(READ mmap): points to file memory mapped by RawImageBackend;
     * IN(READ staging): points to an offset within staging_data_;
     * IN(CSW): points to CSW contents in fallback_data;
     * OUT(WRITE mmap): points to mmap memory, read directly from socket or write directly with splice;
     * OUT(WRITE staging): points to reserved space at the end of staging_data_;
     * OUT(Idle): nullptr, read CBW through fallback_data.
     */
    void *external_buf = nullptr;

    /**
     * @brief Bytes to send in the IN direction
     *
     * Set by MscBulkOnlyHandler in handle_bulk_transfer DataIn/Status;
     * this is send_transfer_data's length parameter. Unused for OUT.
     */
    std::size_t actual_length = 0;

    /**
     * @brief Starting LBA for file I/O
     *
     * send_direct / recv_direct calculate file offset from this and block_size.
     * IN(READ): set to read_lba_ by handle_bulk_transfer DataIn;
     * OUT(WRITE): set to write_lba_ by prepare_out_buffer;
     * CSW / CBW: remains 0 (this path is not triggered when direct_io = false).
     */
    std::uint64_t file_lba = 0;

    /**
     * @brief Byte offset within the LBA
     *
     * Combined with file_lba to determine the exact file position.
     * IN(READ): staging_offset_;
     * OUT(WRITE): write_accumulated_.
     */
    std::size_t file_offset = 0;

    /**
     * @brief Whether to use the zero-copy sendfile / splice path
     *
     * Set true only when external_buf points to RawImageBackend's mmap file memory:
     * - IN(READ mmap): set true in handle_bulk_transfer DataIn;
     * - OUT(WRITE mmap): set true in prepare_out_buffer.
     *
     * CSW / CBW / staging fallback paths are all false; send_transfer_data / recv_transfer_data
     * therefore skip send_direct / recv_direct and use asio reads/writes directly.
     */
    bool direct_io = false;

    /**
     * @brief Transfer direction (CMD_SUBMIT direction, determined at allocation)
     *
     * true=IN (READ/CSW response sends data back), false=OUT (CBW/WRITE/UNMAP receives data).
     * Satisfies TransferOperator::alloc_transfer_handle's direction-recording contract,
     * for transfer_is_in queries. MSC OUT responses carry no transfer and normally
     * do not reach send_transfer_data; this field is mainly for protocol-layer length calculation.
     */
    bool is_in = false;

    // ===== Buffer for this struct's own use =====

    /**
     * @brief Fallback buffer when external_buf is null
     *
     * - OUT(Idle): read CBW (31 bytes) here, then on_out_data_received uses memcpy to copy it into
     *   current_cbw_, preventing overly long CBWs from corrupting the stack;
     * - IN(Status): construct CSW (13 bytes) here, with external_buf pointing to its data().
     */
    std::vector<std::uint8_t> fallback_data;

    /** Reset all fields for object-pool reuse */
    void reset() {
        external_buf = nullptr;
        actual_length = 0;
        file_lba = 0;
        file_offset = 0;
        direct_io = false;
        is_in = false;
        fallback_data.clear();
    }

    static StorageIoTransfer *from_handle(void *handle) {
        return static_cast<StorageIoTransfer *>(handle);
    }
};

// Transfer object type must satisfy FromHandleTransfer constraints (same check as GenericTransfer in protocol.h)
static_assert(FromHandleTransfer<StorageIoTransfer, StorageIoTransfer>);

} // namespace usbipdcpp
