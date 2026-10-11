#pragma once

#include <cstdint>
#include <system_error>
#include <vector>

namespace usbipdcpp {

/**
 * @brief Abstract base class for block storage backends.
 *
 * MscBulkOnlyHandler reads/writes disk blocks through this interface, independent of whether storage uses a raw file, qcow2,
 * or another format. Derived classes implement read/write/block_count.
 */
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    /** @return The number of bytes actually read */
    virtual std::size_t read(std::uint64_t lba, std::uint16_t count, void *buffer) = 0;
    /** @return The number of bytes actually written */
    virtual std::size_t write(std::uint64_t lba, std::uint16_t count, const void *data) = 0;

    // Release physical storage for an LBA range (optional, default no-op)
    virtual void punch_hole(std::uint64_t lba, std::uint64_t count) {
    }

    // Return a direct-read pointer to mapped memory at LBA (nullptr means no mmap, requiring staging_data_ intermediate buffering)
    virtual void *get_direct_buffer(std::uint64_t lba) {
        return nullptr;
    }

    // Zero-copy send (sendfile / TransmitFile), default false → fall back to asio::write
    virtual bool send_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                             std::error_code &ec) {
        return false;
    }

    // Zero-copy receive (splice sock→pipe→file), default false → fall back to asio::read
    virtual bool recv_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                             std::error_code &ec) {
        return false;
    }

    [[nodiscard]] virtual std::uint64_t block_count() const = 0;

    [[nodiscard]] virtual std::uint32_t block_size() const {
        return 512;
    }
};

} // namespace usbipdcpp
