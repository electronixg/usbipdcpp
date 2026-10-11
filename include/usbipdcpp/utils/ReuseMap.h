#pragma once

#include <cstddef>
#include <vector>
#include <utility>

namespace usbipdcpp {

/**
 * @brief Map based on std::vector; deletion marks a slot as empty,
 *        the next insertion reuses an empty slot, expanding with push_back only when no empty slots remain.
 *        linear search O(n), suitable for small-capacity and high-frequency insert/erase scenarios.
 */
template<typename Key, typename Value>
class ReuseMap {
    struct Slot {
        Key key{};
        Value value{};
        bool occupied = false;
    };

    std::vector<Slot> slots_;
    size_t size_ = 0;

public:
    /// Pre-allocate memory to avoid reallocate in subsequent push_back
    void reserve(size_t n) { slots_.reserve(n); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    Value *insert(const Key &key, Value value) {
        // first check whether it already exists
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        // Reuse an empty slot
        for (auto &slot : slots_) {
            if (!slot.occupied) {
                slot.key = key;
                slot.value = std::move(value);
                slot.occupied = true;
                size_++;
                return &slot.value;
            }
        }
        // No empty slot, capacity expansion
        slots_.push_back({key, std::move(value), true});
        size_++;
        return &slots_.back().value;
    }

    Value *find(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    const Value *find(const Key &key) const {
        for (const auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    bool erase(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) {
                slot.occupied = false;
                size_--;
                return true;
            }
        }
        return false;
    }

    template<typename F>
    void for_each(F &&f) {
        for (auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    template<typename F>
    void for_each(F &&f) const {
        for (const auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    void clear() {
        for (auto &slot : slots_) slot.occupied = false;
        size_ = 0;
    }
};

}  // namespace usbipdcpp