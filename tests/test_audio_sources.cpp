// Audio source tests: FourierSource harmonic summation, frequency components, phase, sample-rate switching
//(AudioFileSource moved with its implementation to examples/mock_audio; its tests are in examples/mock_audio/test_audio_file_source.cpp)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

using namespace usbipdcpp;

// ==================== FourierSource ====================

namespace {

/// Goertzel algorithm for detecting the power of a single-frequency component (relative value, used to determine presence)
double goertzel_power(const std::int16_t *samples, std::size_t n, double freq, std::uint32_t rate) {
    double w = 2.0 * std::numbers::pi * freq / rate;
    double coeff = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s0 = samples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

} // namespace

TEST(FourierSource, SingleHarmonicMatchesSine) {
    // A single 440Hz harmonic at full scale (phase 0) approximately matches SineWaveSource output (±1 LSB)
    FourierSource fourier({{440, 1.0}}, {8000}, 1);
    SineWaveSource sine(440, {8000}, 1, 1.0);

    AudioChunk a, b;
    ASSERT_TRUE(fourier.get_chunk(a));
    ASSERT_TRUE(sine.get_chunk(b));
    ASSERT_EQ(a.size, b.size);

    auto *pa = reinterpret_cast<const std::int16_t *>(a.data);
    auto *pb = reinterpret_cast<const std::int16_t *>(b.data);
    int diff_count = 0;
    for (std::size_t i = 0; i < a.size / 2; ++i) {
        if (std::abs(pa[i] - pb[i]) > 1)
            ++diff_count;
    }
    EXPECT_EQ(diff_count, 0);
}

TEST(FourierSource, HarmonicsDetected) {
    // 440:50 + 880:25 → the power of both frequency components should be significantly higher than nearby absent frequencies
    FourierSource src({{440, 0.5}, {880, 0.25}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    auto p440 = goertzel_power(p, n, 440, 8000);
    auto p880 = goertzel_power(p, n, 880, 8000);
    auto p500 = goertzel_power(p, n, 500, 8000); // Absent component as a reference
    EXPECT_GT(p440, p500 * 100);
    EXPECT_GT(p880, p500 * 100);
}

TEST(FourierSource, PhaseShift) {
    // 440Hz with phase π/2: y(t) = sin(2πft + π/2) = cos(2πft)
    // At sample 0, cos(0)=1 → full-scale positive peak; at sample 8, cos(2π·0.44)≈-0.93 → near negative full scale
    FourierSource src({{440, 1.0, 1.5707963267948966}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);

    EXPECT_GT(p[0], 32000);
    EXPECT_LT(p[8], -30000);
    // Reference: at phase 0, sin(0)=0 at sample 0
    FourierSource no_phase({{440, 1.0, 0.0}}, {8000}, 1);
    AudioChunk chunk2;
    ASSERT_TRUE(no_phase.get_chunk(chunk2));
    auto *p2 = reinterpret_cast<const std::int16_t *>(chunk2.data);
    EXPECT_NEAR(p2[0], 0, 400);
}

TEST(FourierSource, ResampleToDifferentRate) {
    FourierSource src({{440, 0.5}}, {8000, 16000}, 1);
    ASSERT_TRUE(src.set_format(1, 16, 16000));
    EXPECT_EQ(src.current_format().sample_rate, 16000);

    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    EXPECT_EQ(chunk.size, 16000 * 2); // 1 second, 16kHz, mono, 16-bit
}

TEST(FourierSource, InvalidHarmonicsFiltered) {
    // NaN/Inf amplitudes and phases are removed (otherwise converting sin(NaN) to an integer is undefined behavior); remaining valid entries produce sound normally
    double nan = std::numeric_limits<double>::quiet_NaN();
    double inf = std::numeric_limits<double>::infinity();
    FourierSource src({{440, 0.5}, {0, 1.0}, {880, nan}, {1000, 0.5, inf}, {1100, -inf}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);

    // All samples must be finite integers (invalid entries have not contaminated the output)
    for (std::size_t i = 0; i < chunk.size / 2; ++i) {
        EXPECT_GE(p[i], -32767);
        EXPECT_LE(p[i], 32767);
    }
    // The 440Hz component is still present (50% amplitude)
    auto p440 = goertzel_power(p, chunk.size / 2, 440, 8000);
    EXPECT_GT(p440, 1.0);
}

TEST(FourierSource, NormalizeOverLimitByDefault) {
    // The summed peak of two harmonics > 1.0 (sin θ + sin 2θ peaks at about 1.76): default normalize mode
    // divides the entire signal by the peak—no clipping (no consecutive full-scale flat tops), with the peak brought to full scale
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0, flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_GT(max_abs, 32760); // Peak brought to full scale (floating-point rounding ±7 LSB)
    EXPECT_LT(flat_count, 3); // No clipped flat tops (clipping produces multiple consecutive full-scale samples within one period)
}

TEST(FourierSource, ClampModeClips) {
    // Clipping mode (normalize=false): samples exceeding the limit are clipped to ±32767, producing many consecutive full-scale flat tops
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1, false);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0, flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_EQ(max_abs, 32767);
    EXPECT_GT(flat_count, 5); // Clipped flat tops are significantly more numerous than in normalization mode
}

TEST(FourierSource, NoScalingBelowLimit) {
    // No scaling when the peak does not exceed full scale: 50% amplitude output peaks around 16383 (preserving volume semantics)
    FourierSource src({{440, 0.5}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0;
    for (std::size_t i = 0; i < n; ++i)
        max_abs = std::max(max_abs, std::abs(p[i]));
    EXPECT_GT(max_abs, 16000);
    EXPECT_LT(max_abs, 16500);
}

TEST(FourierSource, NegativeAmplitudeInvertsPhase) {
    // Negative amplitude = phase inversion: outputs for +A and -A are negatives of each other (±1 LSB)
    FourierSource pos({{440, 0.5}}, {8000}, 1);
    FourierSource neg({{440, -0.5}}, {8000}, 1);
    AudioChunk a, b;
    ASSERT_TRUE(pos.get_chunk(a));
    ASSERT_TRUE(neg.get_chunk(b));

    auto *pa = reinterpret_cast<const std::int16_t *>(a.data);
    auto *pb = reinterpret_cast<const std::int16_t *>(b.data);
    int diff_count = 0;
    for (std::size_t i = 0; i < a.size / 2; ++i) {
        if (std::abs(pa[i] + pb[i]) > 1)
            ++diff_count;
    }
    EXPECT_EQ(diff_count, 0);
}

TEST(FourierSource, SetNormalizeRegenerates) {
    // Switching the overflow-prevention scheme after creation should regenerate immediately: first clip (producing consecutive full-scale flat tops),
    // then switch to normalization; the flat tops disappear and the peak is still brought to full scale
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1, false);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(p[i]) == 32767) ++flat_count;
    }
    EXPECT_GT(flat_count, 5); // Clipping mode has many flat tops

    src.set_normalize(true);
    ASSERT_TRUE(src.get_chunk(chunk)); // buffer has been regenerated; obtain the pointer again
    p = reinterpret_cast<const std::int16_t *>(chunk.data);
    int max_abs = 0;
    flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_GT(max_abs, 32760); // Peak brought to full scale
    EXPECT_LT(flat_count, 3); // Flat tops disappear (no clipping)
}
