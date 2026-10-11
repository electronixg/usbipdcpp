#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "usbipdcpp/utils/RingBuffer.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

struct ma_device; // forward declaration (miniaudio’s typedef is in the global namespace): the header file does not expose the miniaudio type

namespace usbipdcpp {

/// Audio sink for local sound-card playback (based on miniaudio; a pull-model callback reads the ring buffer to feed the sound card,
/// Fill in mute when the buffer is insufficient). Restart the playback device when the sampling rate/channel negotiation changes, and the resampling is handled by the sound card driver.
/// Failed to open the playback device (no sound card, etc.) without throwing an exception: enter the discard mode after logging the error
/// (write_pcm only counts), the device can still be enumerated normally
class PlaybackSink : public AudioSink {
public:
    /// @param device_name Playback device name (miniaudio enumeration name, default empty = system default output device)
    explicit PlaybackSink(std::string device_name = {});
    ~PlaybackSink() override;

    // holds the original pointer of ma_device, copying is prohibited (shallow copy will double release)
    PlaybackSink(const PlaybackSink &) = delete;
    PlaybackSink &operator=(const PlaybackSink &) = delete;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    void write_pcm(const std::uint8_t *data, std::size_t size) override;
    void reset() override;

    /// Total number of PCM bytes received (for statistics)
    [[nodiscard]] std::uint64_t received_bytes() const;

private:
    void open_device(); // Open the playback device according to the current format; fail and enter discard mode (the caller must already hold mutex)
    void close_device();
    // miniaudio pull model callback (ma_device_data_proc signature)
    static void data_callback(struct ma_device *device, void *output, const void *input,
                              std::uint32_t frame_count);

    std::string device_name;
    AudioFormatInfo format{1, 16, 48000};
    mutable std::mutex mutex; // Streaming thread write buffer vs playback callback read buffer
    // 512KB is about 2.7s @96kHz mono (0.7s @48kHz stereo): jitter headroom for host URB bursts and
    // Jitter margin for network peaks; if full, new data will be lost (callback fills in silence)
    RingBuffer buffer{512 * 1024};
    std::uint64_t received = 0;
    bool discarding = false; // Playback device unavailable: count only

    void *device = nullptr; // ma_device* (new allocation, destruction release)
    bool device_initialized = false; // Call uninit only after ma_device_init succeeds
    void *context = nullptr; // ma_context* (self-built when --device specifies the name, it needs to live longer than device)
};

} // namespace usbipdcpp
