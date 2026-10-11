#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "usbipdcpp/Export.h"

namespace usbipdcpp {

/**
 * @brief Abstract Ethernet backend interface
 *
 * Symmetric with AudioSource / AudioSink (audio front/backends): implementations handle Ethernet frame transmission/reception.
 * EcmDataInterfaceHandler passes host-sent frames to send_frame (push to the "network side");
 * when the backend produces frames for the host, it pushes them to the handler through the send_to_host callback.
 * Both callback directions are "inbound" (actively driven by the consumer), without a pull model: the network adapter's data plane is an asynchronous event stream.
 */
class USBIPDCPP_API NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    /**
     * @brief Host sends an Ethernet frame through USB (called by the bulk OUT receive-stream thread)
     * @param data Frame data (Ethernet frame, including 14-byte header)
     * @param size Frame length
     * @note Called by the USB receive-stream thread; implementations must return quickly and ensure thread safety (same
     * constraints as AudioSink::write_pcm). Implementations must call count_rx(size) at the beginning
     */
    virtual void send_frame(const std::uint8_t *data, std::size_t size) = 0;

    /// Clear buffers on disconnection/stream stop, ensuring the next connection contains no old data (default no-op)
    virtual void reset() {}

    /// Set the callback for sending to the host (injected by the EcmDataInterfaceHandler constructor, internal use)
    void set_send_to_host(std::function<void(const std::uint8_t *, std::size_t)> callback) {
        send_to_host_ = std::move(callback);
    }

    // ========== Frame statistics (debug/example display, atomic counts, queryable from any thread) ==========

    /// Frames received from the host (accumulated by send_frame)
    [[nodiscard]] std::uint64_t rx_frames() const {
        return rx_frames_;
    }
    /// Bytes received from the host
    [[nodiscard]] std::uint64_t rx_bytes() const {
        return rx_bytes_;
    }
    /// Frames sent to the host (accumulated by send_to_host)
    [[nodiscard]] std::uint64_t tx_frames() const {
        return tx_frames_;
    }
    /// Bytes sent to the host
    [[nodiscard]] std::uint64_t tx_bytes() const {
        return tx_bytes_;
    }

protected:
    /// Must be called at the beginning of send_frame: accumulate statistics for frames received from the host
    void count_rx(std::size_t size) {
        rx_frames_++;
        rx_bytes_ += size;
    }

    /// When the backend produces a frame, call this to send it to the host (silently discard if no callback has been injected)
    void send_to_host(const std::uint8_t *data, std::size_t size) {
        if (send_to_host_) {
            send_to_host_(data, size);
            tx_frames_++;
            tx_bytes_ += size;
        }
    }

private:
    std::function<void(const std::uint8_t *, std::size_t)> send_to_host_;
    std::atomic<std::uint64_t> rx_frames_{0};
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_frames_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
};

} // namespace usbipdcpp
