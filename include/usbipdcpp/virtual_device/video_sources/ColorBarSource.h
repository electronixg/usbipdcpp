#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// SMPTE color-bar test-pattern generator (uncompressed YUY2 format)
class USBIPDCPP_API ColorBarSource : public VideoSource {
public:
    /// @param width  Frame width (default 640)
    /// @param height Frame height (default 480)
    /// @param fps    Frame rate (default 30)
    ColorBarSource(std::uint16_t width = 640, std::uint16_t height = 480, std::uint8_t fps = 30);

    std::vector<VideoFormatInfo> supported_formats() const override;
    VideoFormatInfo current_format() const override;
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override;
    bool get_frame(VideoFrame &frame) override;
    std::size_t max_frame_size() const override;
    std::uint32_t frame_interval() const override;

private:
    void generate_color_bars();

    std::uint16_t width_;
    std::uint16_t height_;
    std::uint32_t frame_interval_; // 100ns units
    std::vector<std::uint8_t> buffer_;
};

} // namespace usbipdcpp
