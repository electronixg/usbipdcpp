#pragma once

#include <limits>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace usbipdcpp {
class StringPool {
public:
    std::uint8_t new_string(const std::wstring &str) {
        std::lock_guard lock(string_pool_mutex);
        // 1..255 are all assignable (0 is a reserved Language ID index);
        // loop variable uses uint16_t: uint8_t will wrap around to 0 when it is incremented at 255, and the condition will never be satisfied.
        for (std::uint16_t index = 1; index <= std::numeric_limits<std::uint8_t>::max(); ++index) {
            if (!string_pool.contains(static_cast<std::uint8_t>(index))) {
                string_pool[static_cast<std::uint8_t>(index)] = str;
                return static_cast<std::uint8_t>(index);
            }
        }
        SPDLOG_CRITICAL("The string pool is exhausted");
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space));
    }

    std::optional<std::wstring> get_string(std::uint8_t index) {
        std::shared_lock lock(string_pool_mutex);
        if (!string_pool.contains(index)) {
            return std::nullopt;
        }
        return string_pool[index];
    }

    void change_string(std::uint8_t index, const std::wstring &new_str) {
        std::lock_guard lock(string_pool_mutex);
        if (!string_pool.contains(index)) {
            SPDLOG_CRITICAL("String index {} is invalid", index);
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }
        string_pool[index] = new_str;
    }

    void remove_string(std::uint8_t index) {
        std::lock_guard lock(string_pool_mutex);
        string_pool.erase(index);
    }

private:
    std::unordered_map<std::uint8_t, std::wstring> string_pool;
    std::shared_mutex string_pool_mutex;
};
}
