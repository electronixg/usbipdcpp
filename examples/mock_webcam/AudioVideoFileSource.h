#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// Audio and video synchronization file source sharing status: single demuxer + audio/video dual decoder.
///
/// Synchronization semantics: master clock = number of audio consumed samples/sampling rate (audio is a continuously consumed clock source),
/// Video frame acquisition is aligned according to the main clock (returns "the frame that should be displayed at the current moment", expired frames are fast forwarded and discarded,
/// Frames that have not yet arrived will not be sent out in advance). Play to the end of the file, the audio and video will loop back to the beginning, and the main clock will return to zero.
///
/// thread security: audio_get_chunk (UAC scheduling thread) and video_get_frame (UVC network
/// thread) concurrent calls, internal mutual exclusion of serialized FFmpeg calls
class AudioVideoFileState {
public:
    /// @param path media file path (must contain both video track and audio track)
    /// @param sample_rates audio output sampling rate list (swresample resampling target, the first one is the initial value)
    explicit AudioVideoFileState(std::string path, std::vector<std::uint32_t> sample_rates);
    ~AudioVideoFileState();

    AudioVideoFileState(const AudioVideoFileState &) = delete;
    AudioVideoFileState &operator=(const AudioVideoFileState &) = delete;

    /// Whether the opening and decoder initialization is successful (missing audio/video tracks are considered failures)
    bool ok() const {
        return init_ok_;
    }

    // ========== VideoSource side (FfmpegVideoSourceView call) ==========

    /// Video supported format (single format: file width and height + file frame rate)
    std::vector<VideoFormatInfo> video_formats() const;

    /// Get the frame that should be displayed at the main clock time (if there is no ready frame, return false, and the caller will process it according to UVC frame loss)
    bool video_get_frame(VideoFrame &frame);

    std::size_t video_max_frame_size() const {
        return max_frame_size_;
    }

    /// File frame interval (100ns unit)
    std::uint32_t video_frame_interval() const {
        return frame_interval_;
    }

    // ========== AudioSource side (called by FfmpegAudioSourceView) ==========

    /// Audio support format (number of file channels × sampling rate list)
    std::vector<AudioFormatInfo> audio_formats() const;

    /// Current negotiation format
    AudioFormatInfo audio_current_format() const;

    /// Switch sample rate (rebuild swresample; channels/bit depth must be consistent with file)
    bool audio_set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate);

    /// Take a piece of PCM (1ms per piece, aligned with AudioFileSource) and advance the main clock
    bool audio_get_chunk(AudioChunk &chunk);

private:
    bool init();
    /// Inside the lock: read the video packet, decode one frame and transfer YUY2 into the pending queue (EOF loop seek)
    bool decode_next_video_frame_locked();
    /// Inside the lock: the video decoder seeks to the target time (flush + clear pending, used for frame tracking by the main clock)
    void seek_video_to_locked(std::int64_t target_us);
    /// In-lock: audio track decoding to S16LE fills dst (insufficient silence compensation), advances the main clock
    void decode_audio_locked(std::int16_t *dst, std::size_t out_samples);
    /// Inside the lock: audio demuxer + decoder seek back to the beginning, the main clock returns to zero (audio loop)
    void seek_to_start_locked();

    std::string path_;
    std::vector<std::uint32_t> sample_rates_;

    // Dual demuxer: audio and video read packet cursors independently. Audio decoding when single demuxer (skipping video packets)
    // will push the cursor to the back of the file, and the video decoding will never catch up (the picture stops at the first frame); the video is independent
    // After  demuxer, you can freely seek and catch up according to the main clock.
    AVFormatContext *fmt_ctx_ = nullptr; // audio only (main clock advancement)
    AVFormatContext *video_fmt_ctx_ = nullptr; // video only (frames taken according to the main clock)
    int video_stream_ = -1;
    int audio_stream_ = -1;
    AVCodecContext *video_codec_ = nullptr;
    AVCodecContext *audio_codec_ = nullptr;
    AVFrame *vframe_ = nullptr;
    AVFrame *yuy2_ = nullptr;
    SwsContext *sws_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVFrame *aframe_ = nullptr;
    AVPacket *packet_ = nullptr; // audio demuxer for reading packets
    AVPacket *video_packet_ = nullptr; // video demuxer for packet reading (isolated from audio)

    // video parameters (taken from the file during init)
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    double fps_ = 0;
    std::uint32_t frame_interval_ = 0;
    std::size_t max_frame_size_ = 0;

    // audio parameters (file channel + current sampling rate)
    std::uint16_t channels_ = 1;
    std::uint32_t sample_rate_ = 48000;
    std::vector<std::uint8_t> chunk_buffer_; // audio_get_chunk 1ms block returned
    std::vector<std::uint8_t> vbuffer_; // video_get_frame Returns the storage of the frame (live until the next call)
    std::vector<std::uint8_t> last_frame_; // Displayed-frame cache: replay while the next frame is not yet due (video waits for audio without blocking)

    std::mutex mutex_;
    // master clock: number of audio consumed samples (audio_get_chunk advances, monotonically increasing)
    std::uint64_t audio_samples_ = 0;
    // audio file duration (µs, AV_TIME_BASE): main clock modulus - video frame position = main clock
    // modulo the file duration, and the audio EOF loop (cursor returns to 0 to refeed) is aligned with the video loop
    std::int64_t audio_duration_us_ = 0;
    // Decoded video frame {pts(µs), YUY2 data}, in decoding order (B frame pts may fall back, tolerated)
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> pending_frames_;
    // video decoding position (pts of last decoded frame, µs): seek catch up when falling behind the main clock beyond tolerance
    std::int64_t last_decoded_pts_us_ = -1;
    // End of video content (pts of the last decoded frame before EOF, µs): The main clock modulo reference is the audio file
    // duration, which may exceed the actual content of the video (last frame of B frame stream pts < container duration) - exceeded
    // There is no video frame to be output in the window. get_frame The frame tracing will loop endlessly. The main clock is clamped here on the frame fetching side.
    std::int64_t video_content_end_us_ = 0;

    bool init_ok_ = false;
};

/// Video side view (VideoSource interface), shares the same file source with the audio view
class FfmpegVideoSourceView : public VideoSource {
public:
    explicit FfmpegVideoSourceView(std::shared_ptr<AudioVideoFileState> state);

    std::vector<VideoFormatInfo> supported_formats() const override;
    VideoFormatInfo current_format() const override;
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override;
    bool get_frame(VideoFrame &frame) override;
    std::size_t max_frame_size() const override;
    std::uint32_t frame_interval() const override;

private:
    std::shared_ptr<AudioVideoFileState> state_;
};

/// Audio side view (AudioSource interface), shares the same file source with the video view
class FfmpegAudioSourceView : public AudioSource {
public:
    explicit FfmpegAudioSourceView(std::shared_ptr<AudioVideoFileState> state);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    std::shared_ptr<AudioVideoFileState> state_;
};

} // namespace usbipdcpp
