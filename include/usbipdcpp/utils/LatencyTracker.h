#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <string>
#include <mutex>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

/**
 * @brief Latency tracker, used to analyze the time taken at each stage of packet processing
 * Thread-safe
 */
class LatencyTracker {
public:
    /**
     * @brief Start tracking the specified seqnum
     */
    void start_tracking(std::uint32_t seqnum) {
        std::lock_guard lock(mutex_);
        tracking_map_[seqnum] = std::chrono::steady_clock::now();
    }

    /**
     * @brief Track and print elapsed time since start
     * @param seqnum serial number
     * @param message format message
     */
    void track(std::uint32_t seqnum, const char* message) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] {} (elapsed: {} us)", seqnum, message, elapsed.count());
    }

    /**
     * @brief End tracking and print the total time
     */
    void end_tracking(std::uint32_t seqnum) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] tracking ended (total: {} us)", seqnum, elapsed.count());
        tracking_map_.erase(it);
    }

    /**
     * @brief End tracking and print custom message and total time
     */
    void end_tracking(std::uint32_t seqnum, const char* message) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] {} (total: {} us)", seqnum, message, elapsed.count());
        tracking_map_.erase(it);
    }

    /**
     * @brief Check whether the specified seqnum is being tracked
     */
    [[nodiscard]] bool is_tracking(std::uint32_t seqnum) const {
        std::lock_guard lock(mutex_);
        return tracking_map_.find(seqnum) != tracking_map_.end();
    }

    /**
     * @brief Clear all tracking
     */
    void clear() {
        std::lock_guard lock(mutex_);
        tracking_map_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> tracking_map_;
};

} // namespace usbipdcpp


// macro definition

// Unified switch: only need #if USBIPDCPP_TRACK_ENABLED for use, no need to write long conditions repeatedly
#if defined(USBIPDCPP_TRACK_PACKAGE) || defined(USBIPDCPP_FORCE_TRACK_PACKAGE)
#define USBIPDCPP_TRACK_ENABLED 1

// Declare delay tracker members in class
#define LATENCY_TRACKER_MEMBER(name) usbipdcpp::LatencyTracker name

// initialization tracking
#define LATENCY_TRACK_START(tracker, seqnum) (tracker).start_tracking(seqnum)

// Track and print elapsed time
#define LATENCY_TRACK(tracker, seqnum, message) (tracker).track(seqnum, message)

// End tracking and print the total time
#define LATENCY_TRACK_END(tracker, seqnum) (tracker).end_tracking(seqnum)

// End tracking and print custom message
#define LATENCY_TRACK_END_MSG(tracker, seqnum, message) (tracker).end_tracking(seqnum, message)

#else
#define USBIPDCPP_TRACK_ENABLED 0

#define LATENCY_TRACKER_MEMBER(name)
#define LATENCY_TRACK_START(tracker, seqnum) ((void)0)
#define LATENCY_TRACK(tracker, seqnum, message) ((void)0)
#define LATENCY_TRACK_END(tracker, seqnum) ((void)0)
#define LATENCY_TRACK_END_MSG(tracker, seqnum, message) ((void)0)

#endif
