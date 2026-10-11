#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

namespace usbipdcpp {

/// Audio sink writing received PCM to a WAV file (16-bit little-endian PCM)
/// Automatically create a file when reception starts; update RIFF and data length fields after each write (the file
/// can be played while running, and forcibly killing the process does not lose the header). Close the current file on format negotiation changes;
/// recreate it on the next receive stream
class USBIPDCPP_API WavFileSink : public AudioSink {
public:
    explicit WavFileSink(std::filesystem::path path);
    ~WavFileSink() override;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    void write_pcm(const std::uint8_t *data, std::size_t size) override;
    void reset() override;

    /// Close the file (idempotent, automatically called by destructor)
    void finalize();

private:
    void open_file();
    void finalize_locked(); // Caller must already hold mutex
    void update_header_locked(); // Update header length fields; caller must already hold mutex
    static void write_u32le(std::ofstream &file, std::uint32_t value);
    static void write_u16le(std::ofstream &file, std::uint16_t value);

    std::filesystem::path path;
    mutable std::mutex mutex; // Receive-thread file writes versus concurrent external finalize
    std::ofstream file;
    AudioFormatInfo format{1, 16, 48000};
    std::uint32_t data_size = 0; // PCM bytes written
    std::uint32_t last_reported_mb = 0; // Last whole-MB value logged (log each threshold only once)
    bool file_open = false;
};

} // namespace usbipdcpp
