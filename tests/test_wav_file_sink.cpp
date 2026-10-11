// WavFileSink unit tests: WAV header writing/backfilling, format negotiation, reset recreating file

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"

using namespace usbipdcpp;

namespace {

/// Separate test file in temporary directory (process isolation; deleted after test)
std::filesystem::path make_test_path(const char *name) {
    // file_clock rep makes to_string overload set ambiguous in some libc++ implementations (termux/mac);
    // explicitly cast to long long before formatting
    auto stamp = static_cast<long long>(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() /
                (std::string(name) + "_" + std::to_string(stamp) + ".wav");
    std::filesystem::remove(path);
    return path;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint32_t read_u32le(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t read_u16le(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

} // namespace

TEST(WavFileSink, WriteThenFinalizeProducesValidWav) {
    auto path = make_test_path("wav_write");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 48000));

        // Write 1ms data (48000Hz mono, 16-bit = 96 bytes)
        std::vector<std::uint8_t> pcm(96, 0xAA);
        sink.write_pcm(pcm.data(), pcm.size());
        sink.finalize();
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    // 44-byte header + 96-byte data
    ASSERT_EQ(bytes.size(), 44u + 96u);
    // RIFF header
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), 4), "RIFF");
    EXPECT_EQ(read_u32le(bytes, 4), 36u + 96u); // chunk size = 36 + data
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 8), 4), "WAVE");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 12), 4), "fmt ");
    EXPECT_EQ(read_u32le(bytes, 16), 16u); // fmt chunk size
    EXPECT_EQ(read_u16le(bytes, 20), 1u); // audio format: PCM
    EXPECT_EQ(read_u16le(bytes, 22), 1u); // channels
    EXPECT_EQ(read_u32le(bytes, 24), 48000u); // sample rate
    EXPECT_EQ(read_u32le(bytes, 28), 48000u * 2); // byte rate
    EXPECT_EQ(read_u16le(bytes, 32), 2u); // block align
    EXPECT_EQ(read_u16le(bytes, 34), 16u); // bits per sample
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 36), 4), "data");
    EXPECT_EQ(read_u32le(bytes, 40), 96u); // data size
    // Data content
    for (std::size_t i = 44; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], 0xAA);
    }
}

TEST(WavFileSink, ResetClosesAndReopens) {
    auto path = make_test_path("wav_reset");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 48000));
        std::vector<std::uint8_t> pcm(48, 0x01);
        sink.write_pcm(pcm.data(), pcm.size());
        sink.reset(); // First stream ends: backfill header and close

        // Second stream: reopen file (overwrite), writing new content
        std::vector<std::uint8_t> pcm2(96, 0x02);
        sink.write_pcm(pcm2.data(), pcm2.size());
        sink.finalize();
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    // Final file contains only second stream's data (overwrite)
    ASSERT_EQ(bytes.size(), 44u + 96u);
    EXPECT_EQ(read_u32le(bytes, 40), 96u);
    for (std::size_t i = 44; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], 0x02);
    }
}

TEST(WavFileSink, SetFormatRejectsUnsupported) {
    auto path = make_test_path("wav_format");
    WavFileSink sink(path);

    EXPECT_FALSE(sink.set_format(1, 8, 48000)); // Only 16-bit supported
    EXPECT_FALSE(sink.set_format(3, 16, 48000)); // Only 1/2 channels supported
    EXPECT_TRUE(sink.set_format(2, 16, 44100));
    EXPECT_EQ(sink.current_format().channels, 2);
    EXPECT_EQ(sink.current_format().sample_rate, 44100u);
    std::filesystem::remove(path);
}

TEST(WavFileSink, DestructorFinalizes) {
    auto path = make_test_path("wav_dtor");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 8000));
        std::vector<std::uint8_t> pcm(16, 0x00);
        sink.write_pcm(pcm.data(), pcm.size());
        // Without explicit finalize, destruction backfills automatically
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    ASSERT_EQ(bytes.size(), 44u + 16u);
    EXPECT_EQ(read_u32le(bytes, 4), 36u + 16u);
    EXPECT_EQ(read_u32le(bytes, 40), 16u);
}
