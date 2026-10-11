#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// audio file source (based on miniaudio, supports WAV/MP3/FLAC/OGG Vorbis, etc.).
/// mock_audio example private class (2024-08 moved from the virtual device library), depends on miniaudio (single header file library,
/// is installed with the miniaudio package of vcpkg; stb_vorbis.c is also in its include directory).
/// Decoded to 16 bit PCM streaming output; supports sampling rate switching, resampling is completed by miniaudio.
/// Loops at the end of the file (can be disabled with loop, and the output will be muted after closing).
/// Initialization failed (the file cannot be opened / the format is not supported / the sampling rate list is illegal) and no exception is thrown:
/// After logging the error log, enter the downgraded state and return to the legal default format (same as FfmpegSource).
/// get_chunk always returns false (the caller fills in mute), and the device can still be enumerated normally.
class AudioFileSource : public AudioSource {
public:
    /// @param path audio file path (WAV/MP3/FLAC/OGG Vorbis)
    /// @param sample_rates Supported sampling rate list (Hz, any positive value, including non-8kHz integer multiples such as 44100,
    ///                     processed by isochronous transfer residual algorithm). When empty, only the file's native sampling rate is supported;
    ///                     When it is not empty, the first one is the initial sampling rate. Any sampling rate in the list is implemented by miniaudio resampling.
    /// @param loop Whether to loop after playing to the end (default loop)
    explicit AudioFileSource(std::string path, std::vector<std::uint32_t> sample_rates = {}, bool loop = true);
    ~AudioFileSource() override;

    // holds the decoder original pointer and prohibits copying (shallow copy will double release); it also avoids the dllexport class
    // from having its implicit copy constructor forcibly instantiated on Windows, which would cause linking to fail in builds without miniaudio.
    AudioFileSource(const AudioFileSource &) = delete;
    AudioFileSource &operator=(const AudioFileSource &) = delete;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    /// (Re)initialize the decoder according to the current negotiated format. Return false on failure
    bool open_decoder();

    std::string path;
    std::vector<std::uint32_t> sample_rates;
    bool loop;

    std::uint16_t file_channels = 1;
    std::uint32_t file_sample_rate = 48000;

    std::uint16_t channels = 1; // Current negotiated channel number (equal to file_channels)
    std::uint32_t sample_rate = 48000; // Current negotiated sampling rate
    bool init_ok = false; // initialization success flag, downgraded to legal default format in case of failure
    bool finished = false; // has finished playing in non-loop mode

    void *decoder = nullptr; // ma_decoder*; do not expose miniaudio in this header
    std::vector<std::uint8_t> chunk_buffer; // get_chunk output buffer (1ms data)
};

} // namespace usbipdcpp
