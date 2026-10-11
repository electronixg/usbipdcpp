#include "mock_cdc_throttle.h"

#include <chrono>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipdcpp;

ThrottleCdcAcmDataInterfaceHandler::ThrottleCdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool, std::uint32_t limit_bytes, std::uint32_t window_ms) :
    CdcAcmDataInterfaceHandler(handle_interface, string_pool), limit_bytes(limit_bytes), window_ms(window_ms) {
    // Defensive check: limit=0 would make every OUT immediately trigger rate limiting, violating the fixed-byte-count semantics
    if (this->limit_bytes == 0) {
        this->limit_bytes = 1;
    }

    // periodic reporting thread: every window_ms/2 milliseconds, the accumulated '1' count is sent back to the host as "digit \n" (IN direction).
    // Independent of NAK timing: suspending reception for rate limiting does not stop statistical reporting
    report_timer = std::jthread([this](std::stop_token st) {
        const auto report_interval = std::chrono::milliseconds(this->window_ms / 2);
        while (!st.stop_requested()) {
            // Wait until the next reporting time (10ms granular polling, disconnection can stop in time)
            auto deadline = std::chrono::steady_clock::now() + report_interval;
            while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (st.stop_requested()) {
                break;
            }
            std::size_t count;
            {
                std::lock_guard lock(state_mutex);
                count = ones_count;
            }
            // is sent back to the host in the format of "digit \n" (non-blocking: discard this time when the tx buffer is full, and wait for the next cycle)
            std::string msg = std::to_string(count) + "\n";
            send_data(msg);
            SPDLOG_INFO("Reporting the '1' count: {}", msg);
        }
    });
}

bool ThrottleCdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    bool start_throttle = false;
    {
        std::lock_guard lock(state_mutex);
        if (throttling) {
            // NAK period: Do not receive, return false to let the handler suspend the request into out_channel
            // (data is already in the device memory, only delayed response; host URB hangs = back pressure stops sending)
            return false;
        }
        // counts the number of '1' characters in this data, and accumulates it to the reporting count
        for (std::uint8_t b: data) {
            if (b == static_cast<std::uint8_t>('1')) {
                ones_count++;
            }
        }
        received_in_window += data.size();
        if (received_in_window >= limit_bytes) {
            // The window is full: This entry is counted and answered immediately, and then the NAK period is entered window_ms milliseconds
            received_in_window = 0;
            throttling = true;
            start_throttle = true;
        }
    }
    if (start_throttle) {
        SPDLOG_INFO("Window has received its limit of {} bytes; pausing reception for {} ms (OUT NAK)", limit_bytes, window_ms);
        arm_throttle_timer();
    }
    // Normal reception in the window (data statistics have been completed), the response is completed by the handler
    return true;
}

void ThrottleCdcAcmDataInterfaceHandler::arm_throttle_timer() {
    // The new timer overwrites the old jthread: If the timer thread in the previous window is still running, it will be joined and wait for it to exit.
    // (The normal situation has already ended by itself, and join returns immediately)
    throttle_timer = std::jthread([this](std::stop_token st) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
        // polling wait window ends; when disconnected on_disconnection will request_stop exit early
        while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (st.stop_requested()) {
            return;  // Disconnection: handler is about to be destroyed and will no longer take the recovery path.
        }
        {
            std::lock_guard lock(state_mutex);
            throttling = false;
        }
        flush_pending_out();
    });
}

void ThrottleCdcAcmDataInterfaceHandler::flush_pending_out() {
    // Take and respond to all OUT requests pending during the NAK period (data is already in device memory; draining them frees the slots).
    // Do not count the '1' characters in data pending during suspension (it did not follow the on_data_received business-statistics path); only drain it to free slots
    std::size_t flushed = 0;
    while (try_take_out()) {
        flushed++;
    }
    if (flushed) {
        SPDLOG_INFO("Rate limiting finished; drain {} pending OUT requests and resume reception", flushed);
    }
    else {
        SPDLOG_INFO("Rate limiting finished; resuming reception");
    }
}

void ThrottleCdcAcmDataInterfaceHandler::on_disconnection(error_code &ec) {
    // Stop two threads first and then clean up the channel: the handler is about to be destroyed, and the scheduled/reporting thread cannot access the destroyed session.
    if (throttle_timer.joinable()) {
        throttle_timer.request_stop();
        throttle_timer.join();
    }
    if (report_timer.joinable()) {
        report_timer.request_stop();
        report_timer.join();
    }
    CdcAcmDataInterfaceHandler::on_disconnection(ec);
}

bool ThrottleCdcAcmDataInterfaceHandler::is_throttling() const {
    std::lock_guard lock(state_mutex);
    return throttling;
}