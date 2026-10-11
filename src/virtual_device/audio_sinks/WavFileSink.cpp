#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"

#include <spdlog/spdlog.h>

namespace usbipdcpp {

WavFileSink::WavFileSink(std::filesystem::path path) : path(std::move(path)) {}

WavFileSink::~WavFileSink() {
    finalize();
}

std::vector<AudioFormatInfo> WavFileSink::supported_formats() const {
    // Common sample rates × mono/stereo channels (16-bit PCM)
    static const std::uint32_t rates[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: rates) {
        fmts.push_back({1, 16, rate});
        fmts.push_back({2, 16, rate});
    }
    return fmts;
}

AudioFormatInfo WavFileSink::current_format() const {
    std::lock_guard lock(mutex);
    return format;
}

bool WavFileSink::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    std::lock_guard lock(mutex);
    // Supports only 16-bit mono/stereo PCM (WAV header and write path are fixed)
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        return false;
    }
    if (channels != format.channels || sample_rate != format.sample_rate) {
        // Format change: close the current file (update header), recreate with the new format on the next receive stream
        finalize_locked();
        format = {channels, bits_per_sample, sample_rate};
    }
    return true;
}

void WavFileSink::write_pcm(const std::uint8_t *data, std::size_t size) {
    std::lock_guard lock(mutex);
    if (!file_open) {
        open_file();
    }
    file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    data_size += static_cast<std::uint32_t>(size);
    // Update header while writing: finalize/destruction only occurs on normal process exit; forcibly killing (TerminateProcess)
    // does not run destructors, leaving data size in the header at placeholder 0 and making the file unplayable
    update_header_locked();
    auto mb = data_size / (1024 * 1024);
    if (mb > last_reported_mb) {
        last_reported_mb = mb;
        SPDLOG_INFO("WavFileSink: {} MB written", mb);
    }
}

void WavFileSink::reset() {
    std::lock_guard lock(mutex);
    finalize_locked();
}

void WavFileSink::finalize() {
    std::lock_guard lock(mutex);
    finalize_locked();
}

void WavFileSink::finalize_locked() {
    if (!file_open) {
        return;
    }
    // Update header, then close
    update_header_locked();
    file.close();
    file_open = false;
    data_size = 0;
    SPDLOG_INFO("WavFileSink: wrote {}", path.string());
}

void WavFileSink::update_header_locked() {
    // Update RIFF chunk size (offset 4) and data size (offset 40)
    auto end = file.tellp();
    file.seekp(4);
    write_u32le(file, 36 + data_size);
    file.seekp(40);
    write_u32le(file, data_size);
    file.seekp(end);
}

void WavFileSink::open_file() {
    // Standard 44-byte PCM WAV header (placeholder size fields, updated on close)
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        SPDLOG_ERROR("WavFileSink: cannot open output file {}", path.string());
        return;
    }
    file.write("RIFF", 4);
    write_u32le(file, 0); // chunk size placeholder
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    write_u32le(file, 16); // fmt chunk size
    write_u16le(file, 1); // audio format: PCM
    write_u16le(file, format.channels);
    write_u32le(file, format.sample_rate);
    write_u32le(file, format.sample_rate * format.channels * 2); // byte rate
    write_u16le(file, static_cast<std::uint16_t>(format.channels * 2)); // block align
    write_u16le(file, format.bits_per_sample);
    file.write("data", 4);
    write_u32le(file, 0); // data size placeholder
    file_open = true;
    SPDLOG_INFO("WavFileSink: starting to write {} ({}ch {}Hz)", path.string(), format.channels, format.sample_rate);
}

void WavFileSink::write_u32le(std::ofstream &file, std::uint32_t value) {
    std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFF),
                             static_cast<std::uint8_t>((value >> 8) & 0xFF),
                             static_cast<std::uint8_t>((value >> 16) & 0xFF),
                             static_cast<std::uint8_t>((value >> 24) & 0xFF)};
    file.write(reinterpret_cast<const char *>(bytes), 4);
}

void WavFileSink::write_u16le(std::ofstream &file, std::uint16_t value) {
    std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>((value >> 8) & 0xFF)};
    file.write(reinterpret_cast<const char *>(bytes), 2);
}

} // namespace usbipdcpp
