#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// One harmonic term in a Fourier series
struct FourierHarmonic {
    std::uint32_t frequency = 0; // Frequency (Hz), must be an integer (ensuring a seamless 1-second loop)
    double amplitude = 0.5; // Amplitude, full-scale ratio -1.0~1.0 (negative means inversion, equivalent to a π phase shift)
    double phase = 0.0; // Initial phase (radians)
};

/// Fourier-series audio source: y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// Pregenerate 1 second of PCM for looped playback; get_chunk returns the whole segment each time.
/// Supports only signed 16-bit PCM; every harmonic frequency must be an integer Hz (exact whole cycles in 1 second, without phase discontinuities at loop boundaries).
/// normalize determines handling when the superimposed peak exceeds full scale: divide all samples by the peak (preserving waveform shape)
/// or clip each sample; the caller controls amplitude.
/// Supports multiple sample rates (switchable with SET_CUR); rates must be integer multiples of 8kHz
/// (high-speed isochronous transfers require an integer number of bytes per microframe).
class USBIPDCPP_API FourierSource : public AudioSource {
public:
    /// @param harmonics Harmonic list (frequency/amplitude/phase); empty produces silence
    /// @param sample_rates Supported sample rates (Hz, default {48000}); the first is the initial sample rate
    /// @param channels Channel count (1 or 2, default 1)
    /// @param normalize Handling peaks exceeding full scale: true (default) divides all samples by the peak
    /// (preserve waveform shape without clipping; leave unchanged when the peak does not exceed full scale, preserving amplitude semantics); false clips each sample
    /// to full scale
    explicit FourierSource(std::vector<FourierHarmonic> harmonics = {}, std::vector<std::uint32_t> sample_rates = {48000},
                           std::uint16_t channels = 1, bool normalize = true);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

    /// Reset overflow prevention (divide all samples by the peak / clip individual samples when exceeding full scale), regenerating PCM immediately
    /// @param normalize true (default) divides all samples by the peak when exceeding full scale (preserves waveform shape
    /// without clipping; leaves unchanged when the peak does not exceed full scale); false clips each sample to full scale
    void set_normalize(bool normalize);

private:
    /// Regenerate 1 second of superimposed PCM
    void regenerate();

    std::vector<FourierHarmonic> harmonics;
    std::vector<std::uint32_t> sample_rates;
    std::uint32_t sample_rate;
    std::uint16_t channels;
    bool normalize;

    std::vector<std::uint8_t> buffer;
};

} // namespace usbipdcpp
