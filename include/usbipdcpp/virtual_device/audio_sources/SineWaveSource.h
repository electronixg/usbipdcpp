#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// Sine-wave test audio source
/// Pregenerate 1 second of PCM for looped playback; get_chunk returns the whole segment each time.
/// Supports only signed 16-bit PCM; frequency must be an integer Hz (exact whole cycles in 1 second, without phase discontinuities at loop boundaries).
/// Supports multiple sample rates (switchable with SET_CUR); rates must be integer multiples of 8kHz
/// (high-speed isochronous transfers require an integer number of bytes per microframe).
class USBIPDCPP_API SineWaveSource : public AudioSource {
public:
    /// @param frequency_hz Sine frequency (integer Hz, default 440)
    /// @param sample_rates Supported sample rates (Hz, default {48000}); the first is the initial sample rate
    /// @param channels Channel count (1 or 2, default 1)
    /// @param amplitude Amplitude, full-scale ratio 0.0~1.0 (default 0.5)
    explicit SineWaveSource(std::uint32_t frequency_hz = 440, std::vector<std::uint32_t> sample_rates = {48000},
                            std::uint16_t channels = 1, double amplitude = 0.5);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    /// Regenerate 1 second of sine-wave PCM
    void regenerate();

    std::uint32_t frequency_hz;
    std::vector<std::uint32_t> sample_rates;
    std::uint32_t sample_rate;
    std::uint16_t channels;
    double amplitude;

    std::vector<std::uint8_t> buffer;
};

} // namespace usbipdcpp
