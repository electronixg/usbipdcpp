#include "usbipdcpp/virtual_device/video_sources/ColorBarSource.h"

#include "usbipdcpp/virtual_device/UvcConstants.h"

namespace usbipdcpp {

static constexpr std::uint32_t INTERVAL_100NS(int fps) {
    return static_cast<std::uint32_t>(10'000'000ULL / fps);
}

ColorBarSource::ColorBarSource(std::uint16_t width, std::uint16_t height, std::uint8_t fps) :
    width_(width), height_(height), frame_interval_(INTERVAL_100NS(fps)) {
    generate_color_bars();
}

std::vector<VideoFormatInfo> ColorBarSource::supported_formats() const {
    // Supports only one fixed frame rate: min equals default, max = min * 10 (can reduce to 1/10 frame rate)
    auto min_iv = frame_interval_;
    auto max_iv = frame_interval_ * 10;
    // (max - min) % min == 0 satisfies usbvideo.sys's divisibility check
    return {
            {UvcFourCC::YUY2, width_, height_, static_cast<std::uint32_t>(width_ * height_ * 2),
             frame_interval_, min_iv, max_iv, 16},
    };
}

VideoFormatInfo ColorBarSource::current_format() const {
    return supported_formats().front();
}

bool ColorBarSource::set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                                std::uint32_t frame_interval) {
    if (fourcc != UvcFourCC::YUY2)
        return false;
    width_ = width;
    height_ = height;
    frame_interval_ = frame_interval;
    generate_color_bars();
    return true;
}

bool ColorBarSource::get_frame(VideoFrame &frame) {
    frame.data = buffer_.data();
    frame.size = buffer_.size();
    frame.is_keyframe = true;
    return true;
}

std::size_t ColorBarSource::max_frame_size() const {
    return buffer_.size();
}

std::uint32_t ColorBarSource::frame_interval() const {
    return frame_interval_;
}

// SMPTE color bars: white yellow cyan green magenta red blue black
// YCbCr BT.601: Y=16..235, CbCr=16..240, center value=128
void ColorBarSource::generate_color_bars() {
    buffer_.resize(static_cast<std::size_t>(width_) * height_ * 2);

    // Y Cb Cr values for the 8 color bars
    static constexpr std::uint8_t colors[8][3] = {
            {235, 128, 128}, // White
            {210, 16, 146}, // Yellow
            {170, 166, 16}, // Cyan
            {145, 54, 34}, // Green
            {106, 202, 222}, // Magenta
            {81, 90, 240}, // Red
            {41, 240, 110}, // Blue
            {16, 128, 128}, // Black
    };

    auto bar_width = width_ / 8;
    auto *dst = buffer_.data();

    for (std::uint16_t y = 0; y < height_; ++y) {
        for (std::uint16_t x = 0; x < width_; x += 2) {
            auto color_idx = (x / bar_width);
            if (color_idx > 7)
                color_idx = 7;
            auto Y0 = colors[color_idx][0];
            auto Cb = colors[color_idx][1];
            auto Y1 = colors[color_idx][0]; // next pixel same color
            auto Cr = colors[color_idx][2];

            *dst++ = Y0;
            *dst++ = Cb;
            *dst++ = Y1;
            *dst++ = Cr;
        }
    }
}

} // namespace usbipdcpp
