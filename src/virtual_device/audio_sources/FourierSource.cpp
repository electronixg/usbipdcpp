#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace usbipdcpp {

FourierSource::FourierSource(std::vector<FourierHarmonic> harmonics, std::vector<std::uint32_t> sample_rates,
                             std::uint16_t channels, bool normalize) :
    harmonics(std::move(harmonics)), sample_rates(std::move(sample_rates)), channels(channels),
    normalize(normalize) {
    // Note: the initializer list has already moved the arguments; use this-> to access members in the function body (arguments are no longer valid)
    // Filter invalid entries: frequencies must be integer Hz (seamless 1-second loop);
    // nonfinite amplitude/phase (NaN/Inf) corrupts the entire output (converting sin(NaN) to integer is undefined behavior), so remove them directly
    std::erase_if(this->harmonics, [](const FourierHarmonic &h) {
        return h.frequency == 0 || !std::isfinite(h.amplitude) || !std::isfinite(h.phase);
    });
    for (auto &h: this->harmonics) {
        // Negative amplitudes are allowed (inversion, equivalent to a π phase shift); a single term with |amplitude| > 1 exceeds full scale
        // and is meaningless (superimposed overflow is handled consistently by normalize/clipping)
        h.amplitude = std::clamp(h.amplitude, -1.0, 1.0);
    }
    // Filter invalid sample rates (retain all positive values: UAC devices support arbitrary sample rates; the historical restriction to
    // multiples of 8kHz silently discarded rates such as 44100 that are not integer multiples of 8kHz; see SineWaveSource comments)
    std::erase_if(this->sample_rates, [](std::uint32_t r) { return r == 0; });
    if (this->sample_rates.empty()) {
        this->sample_rates.push_back(48000);
    }
    // The first is the initial sample rate (first supplied by the user, without sorting: sorting would change the initial value)
    sample_rate = this->sample_rates.front();
    regenerate();
}

std::vector<AudioFormatInfo> FourierSource::supported_formats() const {
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates.size());
    for (auto rate: sample_rates) {
        formats.push_back({channels, 16, rate});
    }
    return formats;
}

AudioFormatInfo FourierSource::current_format() const {
    return {channels, 16, sample_rate};
}

bool FourierSource::set_format(std::uint16_t new_channels, std::uint8_t bits_per_sample,
                               std::uint32_t new_sample_rate) {
    if (bits_per_sample != 16 || (new_channels != 1 && new_channels != 2)) {
        return false;
    }
    if (std::find(sample_rates.begin(), sample_rates.end(), new_sample_rate) == sample_rates.end()) {
        return false;
    }
    channels = new_channels;
    sample_rate = new_sample_rate;
    regenerate();
    return true;
}

bool FourierSource::get_chunk(AudioChunk &chunk) {
    chunk.data = buffer.data();
    chunk.size = buffer.size();
    return true;
}

void FourierSource::set_normalize(bool normalize) {
    if (this->normalize == normalize) {
        // Skip regeneration if the policy has not changed (regenerate produces 1 second of PCM; unnecessary calls are wasteful)
        return;
    }
    this->normalize = normalize;
    regenerate();
}

void FourierSource::regenerate() {
    // 1 second of PCM: integer-Hz harmonic frequencies give exactly whole cycles, ensuring seamless looped playback
    auto total_samples = sample_rate * channels;
    buffer.resize(total_samples * 2);

    const double two_pi = 2.0 * std::numbers::pi;
    auto *dst = reinterpret_cast<std::int16_t *>(buffer.data());
    // First calculate superimposed values for all samples and record the peak; if exceeding full scale, normalize mode
    // divides all samples by the peak (preserve waveform shape, no clipping); clipping mode leaves values for clamp below
    std::vector<double> samples(sample_rate);
    double peak = 0.0;
    for (std::uint32_t sample = 0; sample < sample_rate; ++sample) {
        double value = 0.0;
        for (auto &h: harmonics) {
            // y(t) = Σ A_k·sin(2π·f_k·t + φ_k), with independent initial phase for each harmonic
            value += h.amplitude *
                     std::sin(two_pi * static_cast<double>(h.frequency) * sample / sample_rate + h.phase);
        }
        samples[sample] = value;
        if (std::abs(value) > peak) {
            peak = std::abs(value);
        }
    }
    // Scale only if the peak exceeds full scale: otherwise retain the user-specified amplitude (50% amplitude means 50% volume).
    // After division by the peak, the theoretical peak is 1.0; clamp below only guards against floating-point rounding errors
    if (normalize && peak > 1.0) {
        for (auto &s: samples) {
            s /= peak;
        }
    }
    for (std::uint32_t sample = 0; sample < sample_rate; ++sample) {
        // Clip to full scale to prevent overflow (should not trigger here in normalize mode)
        auto clamped = static_cast<std::int16_t>(
                std::clamp(samples[sample], -1.0, 1.0) * 32767.0);
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            dst[sample * channels + ch] = clamped;
        }
    }
}

} // namespace usbipdcpp
