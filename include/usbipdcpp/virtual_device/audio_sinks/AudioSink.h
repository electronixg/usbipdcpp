#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// Abstract interface for audio frame consumers
/// Symmetric with AudioSource (generates PCM): implementations receive/play/store PCM data,
/// and UacAudioStreamingSinkHandler writes received iso OUT streams to Sink
class USBIPDCPP_API AudioSink {
public:
    virtual ~AudioSink() = default;

    /// Return all formats supported by Sink
    virtual std::vector<AudioFormatInfo> supported_formats() const = 0;

    /// Return the currently negotiated format
    virtual AudioFormatInfo current_format() const = 0;

    /// Change negotiated format (only formats in the supported list; return false on failure)
    virtual bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) = 0;

    /// Write PCM data (called by the iso OUT receive thread; implementation ensures thread safety)
    virtual void write_pcm(const std::uint8_t *data, std::size_t size) = 0;

    /// Clear buffers on disconnection/stream stop so the next connection does not play old data (default no-op)
    virtual void reset() {}
};

} // namespace usbipdcpp
