#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <type_traits>

#include "usbipdcpp/type.h"

namespace usbipdcpp {

/**
 * @brief Wrapping sequence-number comparison (RFC 1982, as with TCP sequence numbers): whether a is newer than b
 *
 * The seqnum of  uint32_t will wrap around under long-term running (such as the global increment of kernel vhci
 * seqnum); after wrapping, a simple < comparison gives the wrong order. This function is reliable for comparison distances < 2^31
 * (the number of pending requests involved in a comparison cannot reach this magnitude)
 */
inline bool seqnum_newer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

inline void if_has_value_than_rethrow(std::exception_ptr e) {
    if (e)
        std::rethrow_exception(e);
}

/**
 * @brief Determine whether the current platform is little-endian (consteval when known at compile time, otherwise detected at runtime)
 */
constexpr bool is_little_endian() {
    if consteval {
        return std::endian::native == std::endian::little;
    }
    else {
        std::uint16_t tmp = 0x1234u;
        return *reinterpret_cast<std::uint8_t *>(&tmp) != 0x12u;
    }
}

/**
 * @brief host -> USB wire format (little-endian).
 * The multibyte fields in the  USB descriptor and audio/video payload are little-endian, as opposed to network byte order.
 * std::endian::native is a compile-time constant, if constexpr directly eliminates irrelevant branches.
 * is returned unchanged on the little-endian platform, and byte swapped on the big-endian platform to ensure that the serialization result is platform-independent.
 */
template<std::unsigned_integral T>
constexpr T htole(T num) {
    if constexpr (std::endian::native == std::endian::little) {
        return num;
    }
    else {
        return std::byteswap(num);
    }
}

/**
 * @brief Append fields to data_type sequentially in USB wire format (little-endian).
 * can only handle unsigned_integral type and supported_data_type type:
 * The integer type calls htole and then appends by sizeof, and the range type directly copies the memory.
 * first precalculates the total size and resizes it once, and then writes it according to the offset to avoid repeated expansion.
 * Used for descriptor construction, in contrast to network.h's vector_append_to_net (big-endian).
 * @tparam Args Type of incoming data
 * @param vec target vector
 * @param args Fields to be appended
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_append_to_le(data_type &vec, const Args &...args) {
    // Precomputed total size
    std::size_t total_size = 0;
    auto calc_size = [&](const auto &arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            total_size += std::size(arg);
        }
        else {
            total_size += sizeof(RawType);
        }
    };
    (calc_size(args), ...);

    // One-time expansion in place
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // is written sequentially according to offset
    auto process = [&](const auto &arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            std::memcpy(vec.data() + offset, arg.data(), std::size(arg));
            offset += std::size(arg);
        }
        else {
            const RawType le_value = htole(arg);
            std::memcpy(vec.data() + offset, &le_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };
    (process(args), ...);
}
}
