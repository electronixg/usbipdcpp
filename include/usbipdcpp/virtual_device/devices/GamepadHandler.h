#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief USB HID gamepad virtual device handler
 *
 * Standard gamepad layout: 16 buttons + D-pad + 4 analog axes (X/Y/Z/Rz).
 * Report format (11 bytes):
 *   [0-1]  Button bitmask (16-bit, little-endian)
 *   [2]    D-pad direction (eight directions 0-7, 0x0F centered)
 *   [3-4]  X axis (int16 LE)
 *   [5-6]  Y axis (int16 LE)
 *   [7-8]  Z axis (int16 LE)
 *   [9-10] Rz axis (int16 LE)
 */
class USBIPDCPP_API GamepadHandler : public HidVirtualInterfaceHandler {
public:
    static constexpr uint8_t NUM_BUTTONS = 16;
    static constexpr uint8_t NUM_AXES = 4;
    static constexpr std::size_t REPORT_SIZE = 2 + 1 + NUM_AXES * 2; // 11 bytes
    static constexpr int16_t AXIS_MIN = -32768;
    static constexpr int16_t AXIS_MAX = 32767;
    static constexpr int16_t AXIS_CENTER = 0;

    /// D-pad direction
    enum class HatDirection : uint8_t {
        North = 0,
        NorthEast = 1,
        East = 2,
        SouthEast = 3,
        South = 4,
        SouthWest = 5,
        West = 6,
        NorthWest = 7,
        Center = 0x0F,
    };

    GamepadHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~GamepadHandler() override = default;

    /**
     * @brief Create a standard HID gamepad interface (descriptor template, no handler bound)
     *
     * Interface definition: HID class (03/00/00), one interrupt IN endpoint.
     * @param in_ep Interrupt IN endpoint address (device→host, gamepad reports)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 16, // 11-byte report + alignment margin
                                           .interval = 8}}},
        };
        return i;
    }

    // ========== HidVirtualInterfaceHandler interface implementation ==========

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;
    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;

    // ========== Button API ==========

    /// Set button state (0~15)
    void set_button(uint8_t index, bool pressed);
    bool get_button(uint8_t index) const;

    /// Press a set of buttons, releasing all others
    void press_buttons(std::initializer_list<uint8_t> indices);

    /// Release all buttons
    void release_all_buttons();

    // ========== D-pad API ==========

    void set_hat(HatDirection dir);
    HatDirection get_hat() const;

    // ========== Analog axis API ==========

    /// Set axis value (0=X, 1=Y, 2=Z, 3=Rz), range AXIS_MIN~AXIS_MAX, center 0
    void set_axis(uint8_t index, int16_t value);
    int16_t get_axis(uint8_t index) const;

    /// Wait for a client connection
    bool wait_for_client(int timeout_ms = -1);

private:
    struct GamepadState {
        uint16_t buttons = 0;
        uint8_t hat = static_cast<uint8_t>(HatDirection::Center);
        std::array<int16_t, NUM_AXES> axes{};

        bool operator==(const GamepadState &) const = default;
    };

    data_type report_descriptor_;

    GamepadState current_state_;
    GamepadState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
