// Vorbis decoding is provided by external stb_vorbis: miniaudio requires STB_VORBIS_INCLUDE_STB_VORBIS_H
// to be defined before compiling the Vorbis backend. Direct inclusion of the .c implementation file is the standard stb integration method.
// Note: Must be placed before all system headers, its internal macros will conflict with Windows SDK headers.
// stb header file layout varies with the platform (vcpkg is in the include root directory, Termux is in the include/stb/ subdirectory),
// uses __has_include to adapt; when neither is available, only OGG decoding is unavailable (WAV/MP3/FLAC is normal)
#if __has_include("stb_vorbis.c")
#include "stb_vorbis.c"
#elif __has_include("stb/stb_vorbis.c")
#include "stb/stb_vorbis.c"
#endif
// stb_vorbis.c leaves its playback-matrix macros (L/C/R) defined, contaminating subsequently included Windows headers (winnt.h has bit-field members with the same names)
#undef L
#undef C
#undef R

#include "AudioFileSource.h"

#include <algorithm>
#include <cstring>

#include "spdlog/spdlog.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace usbipdcpp {

AudioFileSource::AudioFileSource(std::string path, std::vector<std::uint32_t> sample_rates, bool loop) :
    path(std::move(path)), sample_rates(std::move(sample_rates)), loop(loop) {

    // First open the file in its native format and detect the number of channels and sampling rate
    {
        ma_decoder probe;
        auto cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
        if (ma_decoder_init_file(this->path.c_str(), &cfg, &probe) != MA_SUCCESS) {
            SPDLOG_ERROR("Audio file cannot be opened: {}", this->path);
            return;
        }
        file_channels = static_cast<std::uint16_t>(probe.outputChannels);
        file_sample_rate = probe.outputSampleRate;
        ma_decoder_uninit(&probe);
    }

    if (this->sample_rates.empty()) {
        // Native sample rate only
        this->sample_rates.push_back(file_sample_rate);
    }
    // only checks positive numbers: isochronous transfer is used for non-8kHz integer multiple sampling rates (such as 44100) by the residual algorithm
    // processing (base packet length + overflow supplementary frame, packet length is always an integer multiple of the frame size), no need for 8kHz multiple limit
    for (auto r: this->sample_rates) {
        if (r == 0) {
            SPDLOG_ERROR("The sampling rate must be a positive number: {}", r);
            return;
        }
    }

    // The first sampling rate is the initial value (consistent with SineWaveSource)
    channels = file_channels;
    sample_rate = this->sample_rates.front();

    if (!open_decoder()) {
        SPDLOG_ERROR("Decoder initialization failed: {}", this->path);
        return;
    }
    init_ok = true;
}

AudioFileSource::~AudioFileSource() {
    if (decoder) {
        ma_decoder_uninit(static_cast<ma_decoder *>(decoder));
        delete static_cast<ma_decoder *>(decoder);
    }
}

bool AudioFileSource::open_decoder() {
    auto *dec = new ma_decoder{};
    auto cfg = ma_decoder_config_init(ma_format_s16, file_channels, sample_rate);
    if (ma_decoder_init_file(path.c_str(), &cfg, dec) != MA_SUCCESS) {
        delete dec;
        return false;
    }
    if (decoder) {
        ma_decoder_uninit(static_cast<ma_decoder *>(decoder));
        delete static_cast<ma_decoder *>(decoder);
    }
    decoder = dec;
    finished = false;
    return true;
}

std::vector<AudioFormatInfo> AudioFileSource::supported_formats() const {
    if (!init_ok) {
        // returns a legal default format when initialization fails to avoid driver errors caused by all descriptors being zero (same as FfmpegSource approach)
        return {{1, 16, 48000}};
    }
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates.size());
    for (auto rate: sample_rates) {
        formats.push_back({channels, 16, rate});
    }
    return formats;
}

AudioFormatInfo AudioFileSource::current_format() const {
    if (!init_ok) {
        return {1, 16, 48000};
    }
    return {channels, 16, sample_rate};
}

bool AudioFileSource::set_format(std::uint16_t new_channels, std::uint8_t bits_per_sample,
                                 std::uint32_t new_sample_rate) {
    if (!init_ok) {
        // Soft failure status: Accept switching requests consistent with the downgraded format (no-op) to ensure that the driver can open the flow normally (the output is muted)
        return bits_per_sample == 16 && new_channels == 1 && new_sample_rate == 48000;
    }
    if (bits_per_sample != 16 || new_channels != file_channels) {
        return false;
    }
    if (std::find(sample_rates.begin(), sample_rates.end(), new_sample_rate) == sample_rates.end()) {
        return false;
    }
    if (new_sample_rate == sample_rate) {
        return true;
    }

    // records the old playback position, rebuilds the decoder and converts it proportionally to the new sampling rate.
    // Note: Vorbis decoder get_cursor is always 0, position retention is only valid for WAV/MP3/FLAC
    ma_uint64 old_pos = 0;
    if (decoder) {
        ma_decoder_get_cursor_in_pcm_frames(static_cast<ma_decoder *>(decoder), &old_pos);
    }
    auto old_rate = sample_rate;

    sample_rate = new_sample_rate;
    if (!open_decoder()) {
        // rolls back the sampling rate after reconstruction failure to avoid inconsistency between current_format and the actual decoder status
        sample_rate = old_rate;
        return false;
    }
    auto new_pos = static_cast<ma_uint64>(static_cast<double>(old_pos) * new_sample_rate / old_rate);
    ma_decoder_seek_to_pcm_frame(static_cast<ma_decoder *>(decoder), new_pos);
    return true;
}

bool AudioFileSource::get_chunk(AudioChunk &chunk) {
    if (!init_ok) {
        return false;
    }
    auto *dec = static_cast<ma_decoder *>(decoder);
    if (finished) {
        return false;
    }

    // Each block of 1ms data (sampling rate is an integer multiple of 8kHz, exactly divisible)
    auto frames_per_chunk = static_cast<std::size_t>(sample_rate / 1000);
    chunk_buffer.resize(frames_per_chunk * channels * 2);
    auto *dst = reinterpret_cast<std::int16_t *>(chunk_buffer.data());

    std::size_t got = 0;
    bool looped_once = false;
    while (got < frames_per_chunk) {
        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(dec, dst + got * channels, frames_per_chunk - got, &frames_read);
        if (frames_read == 0) {
            if (loop && !looped_once) {
                // Loop playback: go back to the beginning and continue reading (if there is still no data after wrapping the empty file, the output will be muted)
                SPDLOG_INFO("Audio file playback finished; looping from the beginning: {}", path);
                ma_decoder_seek_to_pcm_frame(dec, 0);
                looped_once = true;
                continue;
            }
            finished = true;
            SPDLOG_INFO("Audio file playback finished: {}", path);
            break;
        }
        got += static_cast<std::size_t>(frames_read);
    }

    if (got == 0) {
        return false;
    }
    // If less than 1ms remains at the end, return a short block; the caller consumes chunk.size bytes and treats the rest as silence.
    chunk.data = chunk_buffer.data();
    chunk.size = got * channels * 2;
    return true;
}

} // namespace usbipdcpp
