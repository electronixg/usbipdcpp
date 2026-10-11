#pragma once

#include <bit>
#include <cstdint>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {

// is_little_endian is defined in type.h, as is htole (little-endian serialization for the USB wire format)

template<std::unsigned_integral T>
constexpr T ntoh(T num) {
    if (is_little_endian()) {
        return std::byteswap(num);
    }
    return num;
}

template<std::unsigned_integral T>
constexpr T hton(T num) {
    return ntoh<T>(num);
}


template<typename T>
concept SerializableFromSocket = requires(const T &t, asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {
    { t.to_socket(sock, ec) } -> std::same_as<void>;
    { std::declval<T &>().from_socket(sock) } -> std::same_as<void>;
};

template<typename T>
concept Serializable = requires(const T &t, asio::ip::tcp::socket &sock) {
    { t.to_bytes() } -> supported_data_type;
    { std::declval<T &>().from_socket(sock) } -> std::same_as<void>;
};

template<typename T>
concept is_serializable_can_be_array = requires(T &&t1) {
    requires Serializable<T>;
    requires is_array_data_type<decltype(t1.to_bytes())>;
};

template<typename T>
struct is_serializable_vector_t : std::false_type {};

template<Serializable T>
struct is_serializable_vector_t<std::vector<T>> : std::true_type {};

template<typename T>
concept serializable_vector = is_serializable_vector_t<T>::value;

/**
 * @brief All use sizeof to calculate length
 * @tparam Args type
 * @return total length
 */
template<std::unsigned_integral... Args>
constexpr std::size_t calculate_unsigned_integral_total_size() {
    return (sizeof(std::remove_reference_t<Args>) + ...);
}

/**
 * @brief can only handle unsigned_integral type and array_data_type type.
 * The integer type is calculated using sizeof, and the array type is calculated using std::size
 * @tparam Args type
 * @return total length
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_total_size_with_array() {
    auto calc_type_size = []<typename T>() -> std::size_t {
        using RawType = std::remove_cvref_t<T>;
        if constexpr (is_array_data_type<RawType>) {
            return std::size(RawType{});
        }
        else {
            return sizeof(RawType);
        }
    };
    return (calc_type_size.template operator()<Args>() + ...);
}

/**
 * @brief Version of calculate_total_size_with_array that deduces types automatically from directly passed arguments.
 * can only handle unsigned_integral type and array_data_type type.
 * The integer type is calculated using sizeof, and the array type is calculated using std::size
 * @param args parameters
 * @return length
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_data_total_size_with_array(const Args &...args) {
    return calculate_data_total_size_with_array<Args...>();
}

/**
 * @brief can only handle unsigned_integral type and supported_data_type type. Integer type is calculated with sizeof, supported_data_type
 * type will be calculated using std::size
 * @param arg parameter
 * @return length
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_data_total_size_with_range(const Args &...arg) {
    auto calc_type_size = []<typename T>(const T &t) -> std::size_t {
        if constexpr (supported_data_type<T>) {
            return std::size(t);
        }
        else {
            return sizeof(T);
        }
    };
    return (calc_type_size(arg) + ...);
}

/**
 * @brief Read the integer from the socket, it will read it all at once and then assign the value. ntoh will be called after reading
 * @param sock target socket
 * @param args integer
 */
template<std::unsigned_integral... Args>
void unsigned_integral_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    constexpr auto total_size = calculate_unsigned_integral_total_size<Args...>();

    std::array<std::uint8_t, total_size> buffer;
    asio::read(sock, asio::buffer(buffer));
    std::size_t offset = 0;

    auto process = [&](auto &arg) {
        using RawType = std::remove_reference_t<decltype(arg)>;
        RawType tmp;
        std::memcpy(&tmp, &buffer[offset], sizeof(tmp));
        offset += sizeof(tmp);

        arg = ntoh(tmp);
    };

    (process(args), ...);
}

/**
 * @brief can only handle unsigned_integral type and supported_data_type type.
 * Call ntoh after reading the  integer type, and the supported_data_type type will be read directly.
 * will read and write one by one
 * @tparam Args All types
 * @param sock target socket
 * @param args The data you want to read in
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void data_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    auto process = [&](auto &arg) -> void {
        using RawType = std::remove_reference_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            asio::read(sock, asio::buffer(arg));
        }
        else {
            RawType tmp;
            asio::read(sock, asio::buffer(&tmp, sizeof(tmp)));
            arg = ntoh(tmp);
        }
    };
    (process(args), ...);
}

inline std::uint64_t read_u64(asio::ip::tcp::socket &sock) {
    std::uint64_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint32_t read_u32(asio::ip::tcp::socket &sock) {
    std::uint32_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint16_t read_u16(asio::ip::tcp::socket &sock) {
    std::uint16_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint8_t read_u8(asio::ip::tcp::socket &sock) {
    std::uint8_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

/**
 * @brief Read integers and arrays from the socket. They will read them all at once and then assign them.
 * ntoh will be called after reading the integer type, and the array type will be copied directly.
 * @tparam padding tail padding number of bytes
 * @tparam Args parameter type
 * @param sock target socket
 * @param args The data you want to read (integer reference or array reference)
 */
template<std::size_t padding = 0, typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void unsigned_integral_and_array_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    constexpr auto total_size = calculate_total_size_with_array<Args...>() + padding;

    std::array<std::uint8_t, total_size> buffer;
    asio::read(sock, asio::buffer(buffer));
    std::size_t offset = 0;

    auto process = [&](auto &arg) {
        using RawType = std::remove_reference_t<decltype(arg)>;
        if constexpr (is_array_data_type<RawType>) {
            std::memcpy(arg.data(), &buffer[offset], arg.size());
            offset += arg.size();
        }
        else {
            RawType tmp;
            std::memcpy(&tmp, &buffer[offset], sizeof(tmp));
            offset += sizeof(tmp);
            arg = ntoh(tmp);
        }
    };

    (process(args), ...);
    // padding data has been read but ignored
}

/**
 * @brief Add padding to the array
 * @tparam padding
 * @param array source array
 * @return array after padding
 */
template<std::size_t padding, is_array_data_type Array,
         std::size_t total_size = calculate_total_size_with_array<Array>() + padding>
constexpr array_data_type<total_size> array_add_padding(const Array &array) {
    array_data_type<total_size> result{};
    std::memcpy(result.data(), array.data(), std::size(array));
    return result;
}

/**
 * @brief Convert the vector of serializable objects into a serialized vector
 * @return converted vector
 */
template<serializable_vector T>
    requires is_serializable_can_be_array<typename T::value_type>
data_type serializable_array_range_to_network_data(const T &vec) {
    constexpr std::size_t serializable_size = decltype(vec.begin()->to_bytes()){}.size();
    std::size_t total_size = serializable_size * vec.size();
    data_type ret(total_size, 0u);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        is_array_data_type auto as_bytes = vec[i].to_bytes();
        std::memcpy(ret.data() + offset, as_bytes.data(), as_bytes.size());
        offset += as_bytes.size();
    }
    return ret;
}

/**
 * @brief can only handle unsigned_integral type and array_data_type type.
 * The integer type will be stored in network byte order, and the array type will be copied directly into the memory. Integer calls hton
 * @tparam Args Type of incoming data
 * @param args Data to be converted
 * @return created array
 */
template<typename... Args, std::size_t total_size = calculate_total_size_with_array<Args...>()>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
array_data_type<total_size> to_network_array(const Args &...args) {
    // Create buffer
    array_data_type<total_size> buffer;

    // handles each parameter
    std::size_t offset = 0;
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;

        if constexpr (is_array_data_type<RawType>) {
            std::memcpy(buffer.data() + offset, arg.data(), arg.size());
            offset += arg.size();
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data to buffer
            std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };
    // Expand all parameters
    (process(args), ...);

    return buffer;
}

/**
 * @brief can only handle unsigned_integral type and supported_data_type type.
 * The integer type will be stored in network byte order, and the range type will be copied directly into the memory. Integer calls hton
 * @tparam Args Type of incoming data
 * @param args Data to be converted
 * @return created array
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
std::vector<std::uint8_t> to_network_data(const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // Create buffer
    data_type buffer;
    buffer.resize(total_size);

    // handles each parameter
    std::size_t offset = 0;
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;

        if constexpr (supported_data_type<RawType>) {
            memcpy(buffer.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data to buffer
            std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all parameters
    (process(args), ...);

    return buffer;
}

/**
 * @brief can only handle unsigned_integral type and supported_data_type type.
 * The integer type will be stored in local byte order, and the range type will be copied directly into the memory.
 * @tparam Args
 * @param vec target vector
 * @param args integer reference
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_mem_order_append(data_type &vec, const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // Expand the buffer
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // handles each parameter
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            memcpy(vec.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            // Copy data to buffer
            std::memcpy(vec.data() + offset, &arg, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all parameters
    (process(args), ...);
}

/**
 * @brief can only handle unsigned_integral type and supported_data_type type.
 * The integer type will be stored in network byte order, and the range type will be copied directly into the memory. Integer calls hton
 * @tparam Args
 * @param vec target vector
 * @param args integer reference
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_append_to_net(data_type &vec, const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // expansion buffer
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // handles each parameter
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            memcpy(vec.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data to buffer
            std::memcpy(vec.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all parameters
    (process(args), ...);
}


} // namespace usbipdcpp
