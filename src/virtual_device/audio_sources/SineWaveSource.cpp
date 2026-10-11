#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace usbipdcpp {

SineWaveSource::SineWaveSource(std::uint32_t frequency_hz, std::vector<std::uint32_t> sample_rates,
                               std::uint16_t channels, double amplitude) :
    frequency_hz(frequency_hz), sample_rates(std::move(sample_rates)), channels(channels), amplitude(amplitude) {
    // Note: the initializer list has already moved the arguments; use this-> to access members in the function body (arguments are no longer valid)
    // Filter invalid sample rates (retain all positive values: UAC devices support arbitrary sample rates; the historical restriction to
    // multiples of 8kHz silently discarded rates such as 44100 that are not integer multiples of 8kHz; see set_format comments)
    std::erase_if(this->sample_rates, [](std::uint32_t r) { return r == 0; });
    if (this->sample_rates.empty()) {
        this->sample_rates.push_back(48000);
    }
    // The first is the initial sample rate (first supplied by the user, without sorting: sorting would change the initial value)
    sample_rate = this->sample_rates.front();
    regenerate();
}

std::vector<AudioFormatInfo> SineWaveSource::supported_formats() const {
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates.size());
    for (auto rate: sample_rates) {
        formats.push_back({channels, 16, rate});
    }
    return formats;
}

AudioFormatInfo SineWaveSource::current_format() const {
    return {channels, 16, sample_rate};
}

bool SineWaveSource::set_format(std::uint16_t new_channels, std::uint8_t bits_per_sample,
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

bool SineWaveSource::get_chunk(AudioChunk &chunk) {
    chunk.data = buffer.data();
    chunk.size = buffer.size();
    return true;
}

void SineWaveSource::regenerate() {
    // 1 second of PCM: integer-Hz frequencies give exactly whole cycles, ensuring seamless looped playback
    auto total_samples = sample_rate * channels;
    buffer.resize(total_samples * 2);

    const double max_amp = 32767.0 * amplitude;
    const double two_pi = 2.0 * std::numbers::pi;
    const double phase_step = two_pi * static_cast<double>(frequency_hz) / static_cast<double>(sample_rate);

    auto *dst = reinterpret_cast<std::int16_t *>(buffer.data());
    for (std::uint32_t sample = 0; sample < sample_rate; ++sample) {
        auto value = static_cast<std::int16_t>(max_amp * std::sin(phase_step * sample));
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            dst[sample * channels + ch] = value;
        }
    }
}

} // namespace usbipdcpp
