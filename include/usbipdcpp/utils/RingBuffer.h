#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

#include "usbipdcpp/Export.h"

namespace usbipdcpp {

/**
 * @brief Ring buffer, supports delayed allocation
 */
class USBIPDCPP_API RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 64 * 1024);

    /**
     * @brief Write data
     * @param data data pointer
     * @param size data size
     * @return The number of bytes actually written
     */
    std::size_t write(const std::uint8_t *data, std::size_t size);

    /**
     * @brief Read data
     * @param data data buffer
     * @param max_size Maximum number of bytes read
     * @return The number of bytes actually read
     */
    std::size_t read(std::uint8_t *data, std::size_t max_size);

    /**
     * @brief View data (do not remove)
     * @param data data buffer
     * @param max_size Maximum number of bytes to view
     * @return The number of bytes actually viewed
     */
    std::size_t peek(std::uint8_t *data, std::size_t max_size) const;

    /**
     * @brief Get the current data volume
     * @return Number of bytes used in the buffer
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Get buffer capacity
     * @return total buffer capacity
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief Get the remaining space of the buffer
     * @return Number of bytes available in the buffer
     */
    [[nodiscard]] std::size_t available() const;

    /**
     * @brief Check whether the buffer is empty
     * @return true means empty
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Check if the buffer is full
     * @return true means full
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief Clear buffer
     */
    void clear();

    /**
     * @brief Adjust buffer capacity
     * @param new_capacity new capacity
     */
    void resize(std::size_t new_capacity);

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

}
