#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief Relative-coordinate mouse virtual device handler
 *
 * Provides mouse operation APIs based on relative offsets. Movement accumulates until the host reads;
 * multiple move() calls automatically accumulate, resetting to zero after report transmission.
 *
 * HID report format (6 bytes):
 *   [0]    Buttons (bit0:left, bit1:right, bit2:middle, bit3:side, bit4:extra) + 3 padding bits
 *   [1-2]  X-axis relative movement (-32767~32767, little-endian)
 *   [3-4]  Y-axis relative movement (-32767~32767, little-endian)
 *   [5]    Wheel (-127~127)
 */
class USBIPDCPP_API RelativeMouseHandler : public HidVirtualInterfaceHandler {
public:
    RelativeMouseHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~RelativeMouseHandler() override = default;

    /**
     * @brief Create a standard HID relative mouse interface (descriptor template, no handler bound)
     *
     * Interface definition: HID class (03/00/00), one interrupt IN endpoint.
     * @param in_ep Interrupt IN endpoint address (device→host, mouse reports)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 8, // 6-byte report + alignment margin
                                           .interval = 10}}},
        };
        return i;
    }

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    /// Accumulate relative movement offsets, clamp to [-32767, 32767], continuing to accumulate until the host retrieves them
    void move(std::int16_t dx, std::int16_t dy);
    /// Accumulate wheel offset, clamp to [-127, 127]
    void set_wheel(std::int8_t delta);

    void set_left_button(bool pressed);
    void set_right_button(bool pressed);
    void set_middle_button(bool pressed);
    void set_side_button(bool pressed);
    void set_extra_button(bool pressed);

    /// Press → delay → release
    void left_click(int delay_ms = 50);
    void right_click(int delay_ms = 50);
    void middle_click(int delay_ms = 50);
    void double_click(int delay_ms = 100);

    struct ButtonState {
        bool left = false;
        bool right = false;
        bool middle = false;
        bool side = false;
        bool extra = false;

        bool operator==(const ButtonState &) const = default;
    };
    ButtonState get_button_state() const;

    struct RelativeDataState {
        std::int16_t dx = 0;
        std::int16_t dy = 0;
        std::int8_t wheel = 0;

        bool operator==(const RelativeDataState &) const = default;

        [[nodiscard]] bool all_zeros() const {
            return dx == 0 && dy == 0 && wheel == 0;
        }

        void reset() {
            dx = 0;
            dy = 0;
            wheel = 0;
        }
    };
    RelativeDataState get_relative_data_state() const;

    /// Wait for a client connection
    bool wait_for_client(int timeout_ms = -1);

private:
    struct State {
        ButtonState button;
        RelativeDataState relative_data;

        bool operator==(const State &) const = default;

        [[nodiscard]] const ButtonState &get_button_state() const {
            return button;
        }

        [[nodiscard]] const RelativeDataState &get_relative_data_state() const {
            return relative_data;
        }
    };

    void notify();
    // Send only, without changing button state
    void send_report();

    data_type report_descriptor;

    State current;
    State last;

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    std::thread send_thread;
    std::atomic_bool should_stop{false};

    std::atomic_bool client_connected{false};
    std::mutex connect_mutex;
    std::condition_variable connect_cv;
};

} // namespace usbipdcpp
