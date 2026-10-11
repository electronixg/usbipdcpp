#include "usbipdcpp/utils/RingBuffer.h"

#include <algorithm>

namespace usbipdcpp {

RingBuffer::RingBuffer(std::size_t capacity) :
    capacity_(capacity) {
    // Lazy allocation: do not allocate memory during construction; resize or write triggers allocation
}

std::size_t RingBuffer::write(const std::uint8_t *data, std::size_t size) {
    if (capacity_ == 0) {
        return 0;  // Do not write when capacity is 0
    }

    std::size_t available = capacity_ - count_;
    std::size_t to_write = std::min(size, available);

    // Lazy allocation: allocate memory only on the first write
    if (buffer_.size() < capacity_) {
        buffer_.resize(capacity_);
    }

    for (std::size_t i = 0; i < to_write; ++i) {
        buffer_[tail_] = data[i];
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
    }

    return to_write;
}

std::size_t RingBuffer::read(std::uint8_t *data, std::size_t max_size) {
    std::size_t to_read = std::min(max_size, count_);

    for (std::size_t i = 0; i < to_read; ++i) {
        data[i] = buffer_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;
    }

    return to_read;
}

std::size_t RingBuffer::peek(std::uint8_t *data, std::size_t max_size) const {
    std::size_t to_peek = std::min(max_size, count_);
    std::size_t pos = head_;

    for (std::size_t i = 0; i < to_peek; ++i) {
        data[i] = buffer_[pos];
        pos = (pos + 1) % capacity_;
    }

    return to_peek;
}

std::size_t RingBuffer::size() const {
    return count_;
}

std::size_t RingBuffer::capacity() const {
    return capacity_;
}

std::size_t RingBuffer::available() const {
    return capacity_ - count_;
}

bool RingBuffer::empty() const {
    return count_ == 0;
}

bool RingBuffer::full() const {
    return count_ == capacity_;
}

void RingBuffer::clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

void RingBuffer::resize(std::size_t new_capacity) {
    if (new_capacity == 0) {
        // Clear and release memory
        buffer_.clear();
        buffer_.shrink_to_fit();
        capacity_ = 0;
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        return;
    }

    // Read existing data
    std::vector<std::uint8_t> old_data(count_);
    read(old_data.data(), count_);

    // Release old memory and reallocate to the exact size
    buffer_.clear();
    buffer_.shrink_to_fit();
    buffer_.resize(new_capacity);
    capacity_ = new_capacity;
    head_ = 0;
    tail_ = 0;
    count_ = 0;

    // Write data back (without exceeding the new capacity)
    if (!old_data.empty()) {
        write(old_data.data(), std::min(old_data.size(), new_capacity));
    }
}

}
