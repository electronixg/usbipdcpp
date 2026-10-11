#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbipdcpp {

/// Format information supported by a video source
struct VideoFormatInfo {
    std::uint32_t fourcc; // FOURCC pixel format (UvcFourCC)
    std::uint16_t width; // Frame width
    std::uint16_t height; // Frame height
    std::uint32_t max_frame_size; // Maximum frame bytes
    std::uint32_t default_frame_interval; // Default frame interval (100ns units)
    /// Shortest supported frame interval (100ns units), i.e. highest frame rate.
    /// Shared by Frame descriptor dwMinFrameInterval + PROBE GET_MAX.
    std::uint32_t min_frame_interval;
    /// Longest supported frame interval (100ns units), i.e. lowest frame rate.
    /// Shared by Frame descriptor dwMaxFrameInterval + PROBE GET_MIN.
    /// Constraint: (max - min) % min == 0 (usbvideo.sys divisibility check).
    std::uint32_t max_frame_interval;
    std::uint8_t bits_per_pixel; // Bits per pixel
};

/// Video frame
struct VideoFrame {
    const std::uint8_t *data; // Frame data pointer (lifetime managed by VideoSource)
    std::size_t size; // Frame data size in bytes
    bool is_keyframe; // Whether this is a keyframe
};

/// Abstract interface for video frame sources
/// Implementations generate/read video frames; UvcHandler packets them for UVC protocol transmission
class VideoSource {
public:
    virtual ~VideoSource() = default;

    /// Return all formats supported by the source
    virtual std::vector<VideoFormatInfo> supported_formats() const = 0;

    /// Currently negotiated format
    virtual VideoFormatInfo current_format() const = 0;

    /// Switch format. Only change descriptor indices; UvcHandler calls this after COMMIT
    virtual bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                            std::uint32_t frame_interval) = 0;

    /// Get the next frame. Source manages the data pointer, valid until the next get_frame call
    virtual bool get_frame(VideoFrame &frame) = 0;

    /// Maximum frame size in the current format (for allocating ISO transfer buffers)
    virtual std::size_t max_frame_size() const = 0;

    /// Current format's frame interval (100ns units), for ISO transfer scheduling
    virtual std::uint32_t frame_interval() const = 0;
};

} // namespace usbipdcpp
