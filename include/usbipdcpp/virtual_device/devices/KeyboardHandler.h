#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/// Keyboard modifier mask
namespace KeyboardModifier {
    constexpr std::uint8_t LeftCtrl = 0x01;
    constexpr std::uint8_t LeftShift = 0x02;
    constexpr std::uint8_t LeftAlt = 0x04;
    constexpr std::uint8_t LeftGUI = 0x08;
    constexpr std::uint8_t RightCtrl = 0x10;
    constexpr std::uint8_t RightShift = 0x20;
    constexpr std::uint8_t RightAlt = 0x40;
    constexpr std::uint8_t RightGUI = 0x80;
} // namespace KeyboardModifier

/// Keyboard LED state mask
namespace KeyboardLED {
    constexpr std::uint8_t NumLock = 0x01;
    constexpr std::uint8_t CapsLock = 0x02;
    constexpr std::uint8_t ScrollLock = 0x04;
    constexpr std::uint8_t Compose = 0x08;
    constexpr std::uint8_t Kana = 0x10;
} // namespace KeyboardLED

/**
 * @brief USB HID keyboard virtual device handler (including Consumer Control media keys)
 *
 * Implements the standard USB HID boot keyboard protocol + Consumer Page media control, using Report IDs to separate two independent reports:
 * - Report ID 1: standard 8-byte keyboard report (Boot Protocol compatible)
 * - Report ID 2: 2-byte Consumer Control report (automatically zeroed after one trigger)
 *
 * Supports: up to 6 keys + 8 modifiers, media keys, LED state reception.
 *
 * @note Endpoint max_packet_size must be at least 9 bytes (complete Report ID 1 report)
 */
class USBIPDCPP_API KeyboardHandler : public HidVirtualInterfaceHandler {
public:
    /// Maximum simultaneously pressed keys (excluding modifiers)
    static constexpr std::size_t MAX_KEYS = 6;
    static constexpr std::uint8_t REPORT_ID_KEYBOARD = 1;
    static constexpr std::uint8_t REPORT_ID_CONSUMER = 2;

    KeyboardHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~KeyboardHandler() override = default;

    /**
     * @brief Create a standard HID keyboard interface (descriptor template, no handler bound)
     *
     * Interface definition: HID class, Boot keyboard subclass/protocol (03/01/01), one interrupt IN endpoint.
     * @param in_ep Interrupt IN endpoint address (device→host, keyboard/media-key reports)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                .interface_subclass = 0x01, // Boot Interface Subclass
                .interface_protocol = 0x01, // Keyboard
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 16, // Complete Report ID 1 report requires ≥9 bytes
                                           .interval = 10}}},
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
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length, const data_type &data,
                            std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;

    // ========== Key API ==========

    /**
     * @brief Press a key
     * @param keycode USB HID keycode (such as 0x04 = A, 0x28 = Enter)
     *
     * Ignore if the key is already pressed or all 6 slots are occupied.
     */
    void press_key(std::uint8_t keycode);

    /**
     * @brief Release a key
     * @param keycode USB HID keycode
     */
    void release_key(std::uint8_t keycode);

    /// Release all keys and modifiers
    void release_all();

    /**
     * @brief Press multiple keys simultaneously
     * @param keycodes Keycode list
     *
     * If exceeding MAX_KEYS, use only the first 6.
     */
    void press_keys(std::initializer_list<std::uint8_t> keycodes);

    /// Query whether a key is currently pressed
    [[nodiscard]] bool is_key_pressed(std::uint8_t keycode) const;

    // ========== Modifier API ==========

    /// Set modifiers (bitwise OR of KeyboardModifier constants)
    void set_modifier(std::uint8_t mask);

    /// Clear specified modifiers
    void clear_modifier(std::uint8_t mask);

    /// Get current modifier state
    [[nodiscard]] std::uint8_t get_modifier() const;

    // ========== Media key API (Consumer Control) ==========

    /**
     * @brief Send a media key, automatically releasing it after one trigger
     * @param usage USB Consumer Page Usage ID (such as HIDConsumer::PlayPause)
     */
    void press_media_key(std::uint16_t usage);

    // ========== LED state ==========

    /// Get host-set LED state (bitwise AND with KeyboardLED constants)
    [[nodiscard]] std::uint8_t get_led_status() const;

    /// Wait for a client connection; negative timeout_ms means wait indefinitely
    bool wait_for_client(int timeout_ms = -1);

private:
    struct KeyboardState {
        std::uint8_t modifier = 0;
        std::array<std::uint8_t, MAX_KEYS> keys{};
        std::uint16_t consumer_usage = 0; // Consumer Page Usage ID, 0=no action

        bool operator==(const KeyboardState &) const = default;
    };

    data_type report_descriptor_;
    std::atomic<std::int16_t> idle_speed_{1};
    std::atomic<std::uint8_t> led_status_{0};

    KeyboardState current_state_;
    KeyboardState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
