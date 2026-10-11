#pragma once

#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"

/**
 * @brief Rate-limited serial-port data-interface handler with statistics
 *
 * Two things:
 * 1. Window rate limit: a maximum of limit_bytes bytes can be received within the fixed window (window_ms milliseconds). After exceeding
 *    Entering NAK period suspends reception for window_ms milliseconds (during which OUT request is suspended, host NAK back pressure),
 *    Window end resumes reception and drains pending requests.
 * 2. Statistical reporting: Accumulate the number of '1' characters in host-sent data; every window_ms/2 milliseconds, send the count
 *    (decimal text + '\n') sent back to the host (IN direction).
 *
 * Rate-limiting semantics match kernel u_serial backpressure: when the tty layer cannot accept data, keep it in the request without dropping it and
 * retry later; exhausting read_pool causes the OUT endpoint to NAK. Here, the fixed window triggers this inability to accept data: once the window
 * reaches its limit, on_data_received returns false → the request is pending in out_channel → host NAK
 * stops sending; when the window ends and resumes, all pending requests are taken out and responded (the data is already in the device memory, only the response is delayed)
 */
class ThrottleCdcAcmDataInterfaceHandler : public usbipdcpp::CdcAcmDataInterfaceHandler {
public:
    ThrottleCdcAcmDataInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool,
                                       std::uint32_t limit_bytes, std::uint32_t window_ms);

    /// Create a rate-limited data interface (descriptor template without a bound handler), reusing the base-class CDC data-interface definition
    static usbipdcpp::UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        return usbipdcpp::CdcAcmDataInterfaceHandler::make_interface(in_ep, out_ep);
    }

    bool on_data_received(usbipdcpp::data_type &&data) override;
    void on_disconnection(usbipdcpp::error_code &ec) override;

    /// Check whether it is currently in the NAK period (receiving is currently limited and suspended)
    bool is_throttling() const;

private:
    void arm_throttle_timer();   // starts the NAK period timing thread (window end triggers recovery)
    void flush_pending_out();    // takes away and responds to all OUT requests pending during the NAK period

    std::uint32_t limit_bytes;  // Maximum number of bytes received per window n
    std::uint32_t window_ms;    // window duration (milliseconds), when reception resumes

    mutable std::mutex state_mutex;
    std::size_t received_in_window = 0;  // The current window has received bytes
    std::size_t ones_count = 0;          // The cumulative number of '1' characters received (for reporting)
    bool throttling = false;             // Is  in the NAK period?

    std::jthread throttle_timer;  // NAK period timing thread (request_stop stops when disconnected)
    std::jthread report_timer;    // Periodic reporting thread: send the count every window_ms/2 (stop on disconnection)
};