// AudioFileSource unit test (implementation follows the example, miniaudio is available and compiled when building tests):
// WAV loading, sample rate switching (miniaudio resampling), looping/non-looping, soft failure
// was added by examples/mock_audio/CMakeLists.txt when USBIPDCPP_BUILD_TESTS

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <fstream>
#include <string>
#include <vector>

#include "AudioFileSource.h"

using namespace usbipdcpp;

namespace {

/// Generate 16 bit PCM WAV test file (440Hz sine), return file path
std::string make_test_wav(const std::string &tag, std::uint32_t sample_rate, std::uint16_t channels, double seconds) {
    auto total_frames = static_cast<std::size_t>(sample_rate * seconds);
    std::vector<std::int16_t> samples(total_frames * channels);
    for (std::size_t i = 0; i < total_frames; ++i) {
        auto v = static_cast<std::int16_t>(16384.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / sample_rate));
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            samples[i * channels + ch] = v;
        }
    }

    auto path = (std::filesystem::temp_directory_path() / ("usbipdcpp_test_" + tag + ".wav")).string();

    std::ofstream f(path, std::ios::binary);
    auto put_u16 = [&f](std::uint16_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put_u32 = [&f](std::uint32_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
        f.put(static_cast<char>((v >> 16) & 0xFF));
        f.put(static_cast<char>((v >> 24) & 0xFF));
    };

    auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    f.write("RIFF", 4);
    put_u32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(16); // fmt chunk size
    put_u16(1); // PCM
    put_u16(channels);
    put_u32(sample_rate);
    put_u32(sample_rate * channels * 2); // byte rate
    put_u16(channels * 2); // block align
    put_u16(16); // bits per sample
    f.write("data", 4);
    put_u32(data_bytes);
    f.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(data_bytes));
    f.close();
    return path;
}

} // namespace

TEST(AudioFileSource, LoadWavBasic) {
    auto path = make_test_wav("basic", 8000, 1, 1.0);
    {
        AudioFileSource src(path);

        auto fmt = src.current_format();
        EXPECT_EQ(fmt.sample_rate, 8000);
        EXPECT_EQ(fmt.channels, 1);
        EXPECT_EQ(fmt.bits_per_sample, 16);

        // Each block 1ms: 8 frame × 2 bytes = 16 bytes, and not all zeros
        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 16);
        bool has_nonzero = false;
        for (std::size_t i = 0; i < chunk.size; ++i) {
            if (chunk.data[i] != 0)
                has_nonzero = true;
        }
        EXPECT_TRUE(has_nonzero);
    } // src destructs to release the file handle
    std::filesystem::remove(path);
}

TEST(AudioFileSource, ResampleToDifferentRate) {
    auto path = make_test_wav("resample", 8000, 1, 1.0);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{8000, 16000});

        ASSERT_TRUE(src.set_format(1, 16, 16000));
        EXPECT_EQ(src.current_format().sample_rate, 16000);

        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 32); // 16 frame × 2 byte
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, StereoWav) {
    auto path = make_test_wav("stereo", 16000, 2, 1.0);
    {
        AudioFileSource src(path);

        EXPECT_EQ(src.current_format().channels, 2);
        EXPECT_EQ(src.current_format().sample_rate, 16000);

        // Refuse to switch when the number of channels is inconsistent with the file
        EXPECT_FALSE(src.set_format(1, 16, 16000));
        EXPECT_FALSE(src.set_format(2, 8, 16000));
        // Sampling rate not in the list refuses to switch
        EXPECT_FALSE(src.set_format(2, 16, 32000));

        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 64); // 16 frame × 2 channel × 2 bytes
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, NonLoopStopsAtEnd) {
    // 10ms file, non-looping: get_chunk should return false after 10 calls
    auto path = make_test_wav("nonloop", 8000, 1, 0.01);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{}, false);

        int ok = 0;
        AudioChunk chunk;
        while (ok < 100 && src.get_chunk(chunk)) {
            ++ok;
        }
        EXPECT_EQ(ok, 10);
        EXPECT_FALSE(src.get_chunk(chunk));
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, LoopWrapsAround) {
    auto path = make_test_wav("loop", 8000, 1, 0.01);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{}, true);

        // loop mode: reading far exceeds the file length (1 seconds = 1000 blocks) and still has data
        AudioChunk chunk;
        for (int i = 0; i < 1000; ++i) {
            ASSERT_TRUE(src.get_chunk(chunk));
            EXPECT_EQ(chunk.size, 16);
        }
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, InvalidFileSoftFail) {
    auto path = (std::filesystem::temp_directory_path() / "usbipdcpp_test_invalid.wav").string();
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a wav file";
    }
    {
        // soft failure (same as FfmpegSource): no exception is thrown, legal downgraded format is returned, and no data is output;
        // Switching requests consistent with the downgraded format are accepted as no-op to ensure that the driver can open the stream.
        AudioFileSource src(path);

        auto fmts = src.supported_formats();
        ASSERT_FALSE(fmts.empty());
        EXPECT_EQ(fmts.front().bits_per_sample, 16);
        EXPECT_EQ(src.current_format().sample_rate, 48000);

        EXPECT_TRUE(src.set_format(1, 16, 48000));
        EXPECT_FALSE(src.set_format(1, 16, 16000));
        AudioChunk chunk;
        EXPECT_FALSE(src.get_chunk(chunk));
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, InvalidSampleRateListSoftFail) {
    auto path = make_test_wav("badrate", 8000, 1, 1.0);
    {
        // The sampling rate list contains illegal values (0): Verification failed and soft failed.
        AudioFileSource src(path, std::vector<std::uint32_t>{0});

        AudioChunk chunk;
        EXPECT_FALSE(src.get_chunk(chunk));
        EXPECT_TRUE(src.set_format(1, 16, 48000));
        EXPECT_FALSE(src.set_format(1, 16, 44100));
    }
    std::filesystem::remove(path);
}
