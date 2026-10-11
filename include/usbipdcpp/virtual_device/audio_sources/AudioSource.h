#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"

namespace usbipdcpp {

/// Format information supported by an audio source
struct AudioFormatInfo {
    std::uint16_t channels = 1; // Channel count
    std::uint8_t bits_per_sample = 16; // Bit depth
    std::uint32_t sample_rate = 48000; // Sample rate (Hz)
};

/// PCM audio data chunk
struct AudioChunk {
    const std::uint8_t *data = nullptr; // Data pointer (lifetime managed by AudioSource)
    std::size_t size = 0; // Data size in bytes
};

/// Abstract interface for audio frame sources
/// Implementations generate/read PCM data; UacHandler packets it for UAC protocol transmission
class USBIPDCPP_API AudioSource {
public:
    virtual ~AudioSource() = default;

    /// Return all formats supported by the source
    virtual std::vector<AudioFormatInfo> supported_formats() const = 0;

    /// Return the currently negotiated format
    virtual AudioFormatInfo current_format() const = 0;

    /// Change negotiated format (only formats in the supported list; return false on failure)
    virtual bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) = 0;

    /// Pull a PCM data chunk.
    /// The returned data pointer is valid until the next get_chunk call; the caller can use it in that interval without freeing it.
    /// Return false when no data is available (caller should fill with silence).
    virtual bool get_chunk(AudioChunk &chunk) = 0;
};

} // namespace usbipdcpp
