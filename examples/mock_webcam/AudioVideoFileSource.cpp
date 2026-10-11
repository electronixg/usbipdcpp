#include "AudioVideoFileSource.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "usbipdcpp/virtual_device/UvcConstants.h"

namespace usbipdcpp {

static constexpr std::uint32_t INTERVAL_100NS(double fps) {
    return static_cast<std::uint32_t>(10'000'000.0 / fps);
}

// ==================== AudioVideoFileState ====================

AudioVideoFileState::AudioVideoFileState(std::string path, std::vector<std::uint32_t> sample_rates) :
    path_(std::move(path)), sample_rates_(std::move(sample_rates)) {
    if (sample_rates_.empty()) {
        SPDLOG_ERROR("AudioVideoFileState: sample rate list must not be empty");
        return;
    }
    sample_rate_ = sample_rates_.front();
    packet_ = av_packet_alloc();
    video_packet_ = av_packet_alloc();
    if (!packet_ || !video_packet_) {
        SPDLOG_ERROR("FFmpeg: failed to allocate packet");
        return;
    }
    if (!init())
        SPDLOG_ERROR("FFmpeg: failed to open {}", path_);
}

AudioVideoFileState::~AudioVideoFileState() {
    sws_freeContext(sws_);
    swr_free(&swr_);
    av_frame_free(&yuy2_);
    av_frame_free(&aframe_);
    av_frame_free(&vframe_);
    avcodec_free_context(&audio_codec_);
    avcodec_free_context(&video_codec_);
    avformat_close_input(&video_fmt_ctx_);
    avformat_close_input(&fmt_ctx_);
    av_packet_free(&video_packet_);
    av_packet_free(&packet_);
}

bool AudioVideoFileState::init() {
    if (avformat_open_input(&fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open {}", path_);
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot find stream info");
        return false;
    }
    // Video-specific demuxer: independent cursors from the audio demuxer (audio and video advance at their own pace,
    // does not interfere with each other; the video is free to seek and catch up according to the main clock)
    if (avformat_open_input(&video_fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open {} (video demuxer)", path_);
        return false;
    }
    if (avformat_find_stream_info(video_fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot find stream info (video demuxer)");
        return false;
    }

    for (unsigned i = 0; i < video_fmt_ctx_->nb_streams; ++i) {
        if (video_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_ = static_cast<int>(i);
            break;
        }
    }
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_ = static_cast<int>(i);
            break;
        }
    }
    // Prerequisite for audio and video synchronization: the file must have both a video track and an audio track
    if (video_stream_ < 0) {
        SPDLOG_ERROR("FFmpeg: no video stream found in {}", path_);
        return false;
    }
    if (audio_stream_ < 0) {
        SPDLOG_ERROR("FFmpeg: no audio stream found in {} (Audio track is required for audio and video synchronization)", path_);
        return false;
    }

    // ---- Video decoder + swscale (aligned to YUY2 path of FfmpegSource) ----
    auto *vpar = video_fmt_ctx_->streams[video_stream_]->codecpar;
    width_ = static_cast<std::uint16_t>(vpar->width);
    height_ = static_cast<std::uint16_t>(vpar->height);
    auto r_frame_rate = video_fmt_ctx_->streams[video_stream_]->r_frame_rate;
    fps_ = av_q2d(r_frame_rate);
    if (fps_ <= 0)
        fps_ = 30.0;
    frame_interval_ = INTERVAL_100NS(fps_);

    auto *vcodec = avcodec_find_decoder(vpar->codec_id);
    if (!vcodec) {
        SPDLOG_ERROR("FFmpeg: unsupported video codec");
        return false;
    }
    video_codec_ = avcodec_alloc_context3(vcodec);
    vframe_ = av_frame_alloc();
    yuy2_ = av_frame_alloc();
    if (!video_codec_ || !vframe_ || !yuy2_)
        return false;
    if (avcodec_parameters_to_context(video_codec_, vpar) < 0)
        return false;
    if (avcodec_open2(video_codec_, vcodec, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open video codec");
        return false;
    }

    yuy2_->format = AV_PIX_FMT_YUYV422;
    yuy2_->width = width_;
    yuy2_->height = height_;
    if (av_image_alloc(yuy2_->data, yuy2_->linesize, width_, height_, AV_PIX_FMT_YUYV422, 1) < 0)
        return false;
    sws_ = sws_getContext(width_, height_, video_codec_->pix_fmt, width_, height_, AV_PIX_FMT_YUYV422,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        SPDLOG_ERROR("FFmpeg: cannot create swscale context");
        return false;
    }
    max_frame_size_ = static_cast<std::size_t>(width_) * height_ * 2;

    // ---- Audio decoder + swresample (output S16LE × file channel × current sampling rate) ----
    auto *apar = fmt_ctx_->streams[audio_stream_]->codecpar;
    audio_duration_us_ = fmt_ctx_->duration > 0 ? fmt_ctx_->duration : 0;
    channels_ = static_cast<std::uint16_t>(apar->ch_layout.nb_channels > 0 ? apar->ch_layout.nb_channels : 1);
    auto *acodec = avcodec_find_decoder(apar->codec_id);
    if (!acodec) {
        SPDLOG_ERROR("FFmpeg: unsupported audio codec");
        return false;
    }
    audio_codec_ = avcodec_alloc_context3(acodec);
    aframe_ = av_frame_alloc();
    if (!audio_codec_ || !aframe_)
        return false;
    if (avcodec_parameters_to_context(audio_codec_, apar) < 0)
        return false;
    if (avcodec_open2(audio_codec_, acodec, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open audio codec");
        return false;
    }

    // output channel layout: file channel (av_channel_layout_default supports 1/2 channels)
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels_);
    if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, sample_rate_, &audio_codec_->ch_layout,
                            audio_codec_->sample_fmt, audio_codec_->sample_rate, 0, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot create swresample context");
        return false;
    }
    if (swr_init(swr_) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot init swresample");
        return false;
    }

    auto codec_name = avcodec_get_name(vpar->codec_id);
    SPDLOG_INFO("FFmpeg: {} — video {}x{} @ {:.2f}fps, audio {}ch {}Hz → S16LE {}Hz", path_, width_, height_,
                fps_, channels_, audio_codec_->sample_rate, sample_rate_);

    init_ok_ = true;
    return true;
}

std::vector<VideoFormatInfo> AudioVideoFileState::video_formats() const {
    if (!init_ok_) {
        // When  init fails, it returns a legal YUY2 format to avoid driver errors caused by all descriptors being zero (same as FfmpegSource)
        return {{UvcFourCC::YUY2, 320, 240, 320u * 240 * 2, 333333u, 333333u, 3333333u, 16}};
    }
    auto min_iv = frame_interval_;
    auto max_iv = frame_interval_ * 10;
    return {{UvcFourCC::YUY2, width_, height_, static_cast<std::uint32_t>(max_frame_size_), frame_interval_,
             min_iv, max_iv, 16}};
}

bool AudioVideoFileState::decode_next_video_frame_locked() {
    while (true) {
        int ret = av_read_frame(video_fmt_ctx_, video_packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_INFO("FFmpeg: video ended, looping from start");
                // records the end of video content (the last decoded frame pts before EOF): the main clock modulo reference is
                // The duration of the audio file will exceed the end of the actual video content, and the frame side is relied on to clamp the main clock.
                if (last_decoded_pts_us_ > 0)
                    video_content_end_us_ = last_decoded_pts_us_;
                // Loop back to the start of the file: clear tail frames that have not yet been popped (when EOF occurs, the master-clock snapshot may
                // already be beyond the content end; all tail frames remain <= the master clock, so leaving them would make the next call wait indefinitely for replay)
                pending_frames_.clear();
                // When video loops back to the start of the file, rebuild all video-side objects (demuxer, decoder, sws,
                // frame buffers) exactly as in init. In testing, seeking alone at EOF did not fully reset
                // the mov demuxer cursor (a few packets were read before EOF recurred); reusing the old decoder/
                // sws entered an invalid state (avcodec_send_packet / sws_scale hung internally without returning,
                // and get_frame held the lock, blocking the entire UVC stream). Rebuilding everything gives a completely fresh state; the cost is
                // ~1ms once per file duration, which is negligible
                avcodec_free_context(&video_codec_);
                video_codec_ = nullptr;
                sws_freeContext(sws_);
                sws_ = nullptr;
                av_frame_free(&vframe_);
                vframe_ = nullptr;
                av_frame_free(&yuy2_);
                yuy2_ = nullptr;
                avformat_close_input(&video_fmt_ctx_);
                video_fmt_ctx_ = nullptr;
                video_stream_ = -1;
                if (avformat_open_input(&video_fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0 ||
                    avformat_find_stream_info(video_fmt_ctx_, nullptr) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot reopen {} (video loop)", path_);
                    return false;
                }
                for (unsigned i = 0; i < video_fmt_ctx_->nb_streams; ++i) {
                    if (video_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        video_stream_ = static_cast<int>(i);
                        break;
                    }
                }
                if (video_stream_ < 0) {
                    SPDLOG_ERROR("FFmpeg: no video stream after reopen");
                    return false;
                }
                // reconstruction decoder + sws + frame buffer (the same file parameters remain unchanged, width and height are used)
                auto *vpar = video_fmt_ctx_->streams[video_stream_]->codecpar;
                auto *vcodec = avcodec_find_decoder(vpar->codec_id);
                vframe_ = av_frame_alloc();
                yuy2_ = av_frame_alloc();
                if (!vcodec || !vframe_ || !yuy2_) {
                    SPDLOG_ERROR("FFmpeg: cannot alloc video loop objects");
                    return false;
                }
                video_codec_ = avcodec_alloc_context3(vcodec);
                if (!video_codec_ || avcodec_parameters_to_context(video_codec_, vpar) < 0 ||
                    avcodec_open2(video_codec_, vcodec, nullptr) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot reopen video codec (loop)");
                    return false;
                }
                yuy2_->format = AV_PIX_FMT_YUYV422;
                yuy2_->width = width_;
                yuy2_->height = height_;
                if (av_image_alloc(yuy2_->data, yuy2_->linesize, width_, height_, AV_PIX_FMT_YUYV422, 1) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot alloc yuy2 buffer (loop)");
                    return false;
                }
                sws_ = sws_getContext(width_, height_, video_codec_->pix_fmt, width_, height_, AV_PIX_FMT_YUYV422,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_) {
                    SPDLOG_ERROR("FFmpeg: cannot create swscale (loop)");
                    return false;
                }
                last_decoded_pts_us_ = -1;
                // The main clock snapshot in get_frame triggered by  EOF may have exceeded the end of the video content.
                // (the modulo reference is the audio duration), so catching up with frames would loop forever; after rebuilding,
                // return immediately. This call outputs the frame already popped/replays the previous frame; the next call uses the clamped master clock
                // to catch up normally from the start of the file
                return false;
            }
            return false;
        }
        if (video_packet_->stream_index != video_stream_) {
            av_packet_unref(video_packet_);
            continue;
        }
        ret = avcodec_send_packet(video_codec_, video_packet_);
        av_packet_unref(video_packet_);
        if (ret < 0)
            continue;
        ret = avcodec_receive_frame(video_codec_, vframe_);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            return false;

        // pts -> microseconds (NOPTS frames are immediately displayable: record pts as -1 and treat the frame as already due when retrieving it)
        std::int64_t pts_us = -1;
        if (vframe_->pts != AV_NOPTS_VALUE) {
            pts_us = av_rescale_q(vframe_->pts, video_fmt_ctx_->streams[video_stream_]->time_base,
                                  AVRational{1, 1'000'000});
        }
        last_decoded_pts_us_ = pts_us;

        std::vector<std::uint8_t> yuy2(max_frame_size_);
        sws_scale(sws_, vframe_->data, vframe_->linesize, 0, height_, yuy2_->data, yuy2_->linesize);
        av_frame_unref(vframe_);

        // lines are copied to the compact buffer (sws output linesize may have alignment padding)
        auto *src = yuy2_->data[0];
        auto line_sz = static_cast<std::size_t>(width_) * 2;
        auto *dst = yuy2.data();
        for (int y = 0; y < height_; ++y) {
            std::memcpy(dst, src, line_sz);
            src += yuy2_->linesize[0];
            dst += line_sz;
        }
        // Insert in pts order: codecs with B frames, such as mpeg4, can have decreasing pts in decoding order; retrieve frames in
        // presentation order (pts) to decide whether to fast-forward or play them
        auto it = std::lower_bound(pending_frames_.begin(), pending_frames_.end(), pts_us,
                                   [](const auto &a, std::int64_t pts) { return a.first < pts; });
        pending_frames_.insert(it, {pts_us, std::move(yuy2)});
        return true;
    }
}

bool AudioVideoFileState::video_get_frame(VideoFrame &frame) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    // master clock (µs): driven by audio consumption; modulo the duration of the audio file - the video frame position and
    // audio loop point alignment (audio EOF loop supply, video EOF loop, each loop period is
    // file duration)
    auto clock_us = static_cast<std::int64_t>(audio_samples_ * 1'000'000ULL / sample_rate_);
    if (audio_duration_us_ > 0)
        clock_us %= audio_duration_us_;
    // The main clock is clamped before the end of the video content: the modulo reference is the audio duration, which will exceed the last frame of the video
    // pts (the last B-frame-stream frame < container duration). Beyond that point, no video frames can be output; the catch-up loop
    // decodes to EOF, rebuilds at 0, and again finds every frame <= clock, so the queue never empties and the lock is held forever. Leave a one-frame margin;
    // video can catch up on both sides of the loop point (EOF rebuilding bridges the last frame omitted from the file tail)
    if (video_content_end_us_ > 0) {
        auto clock_limit = video_content_end_us_ - frame_interval_ / 10;
        if (clock_us > clock_limit)
            clock_us = clock_limit;
    }
    // When the video decoding position lags behind the main clock and exceeds the tolerance, seek catches up (audio streams first/fast forward scenario:
    // sequential decoding cannot catch up; decode sequentially within the tolerance to avoid frequent seeks)
    constexpr std::int64_t SEEK_TOLERANCE_US = 500'000;
    // Whether seek is required for this call: When entering, the decoding position lags behind the main clock and exceeds the tolerance before seeking (audio streams first/
    // fast-forward scenarios). Decide only once on entry: repeated checks in the frame catch-up loop would incorrectly conclude
    // that low-pts frames decoded after seeking are still behind, causing repeated seeks (if the keyframe is at the start of the file, each iteration decodes
    // a pts=0 frame, resulting in an infinite loop)
    bool need_seek = (last_decoded_pts_us_ < 0 || clock_us - last_decoded_pts_us_ > SEEK_TOLERANCE_US);
    // The latest frame already due that was popped during this call: continue decoding after popping it to catch up with the master clock (fast-forward),
    // Until the head of the queue advances or decoding fails - it cannot return every frame, otherwise the video will be chased frame by frame (decoding rate
    // = pull frame rate = playback rate, you will never catch up if you fall behind)
    std::pair<std::int64_t, std::vector<std::uint8_t>> latest{-1, {}};
    bool have_latest = false;
    while (true) {
        if (pending_frames_.empty()) {
            if (need_seek) {
                seek_video_to_locked(clock_us);
                need_seek = false;
            }
            if (!decode_next_video_frame_locked())
                break; // Decoding failure: return the popped frame (if any)
            continue;
        }
        if (pending_frames_.front().first > clock_us)
            break; // Queue head is ahead (its playback time has not arrived): latest is the frame that should currently be displayed
        latest = std::move(pending_frames_.front());
        pending_frames_.erase(pending_frames_.begin());
        have_latest = true;
        // continue: When the queue is empty after popping up, it returns to the beginning of the loop to decode and chase the frame (fast forward to the main clock)
    }
    if (!have_latest) {
        // No arrived frame (theoretically it only occurs when the first frame decoding is advanced after seek): Replay the previous frame
        //(UVC is a pull model, and no frame will cause the driver pull flow to get stuck)
        if (!last_frame_.empty()) {
            frame.data = last_frame_.data();
            frame.size = last_frame_.size();
            frame.is_keyframe = true;
            return true;
        }
        return false;
    }
    last_frame_ = std::move(latest.second);
    vbuffer_ = last_frame_;
    frame.data = vbuffer_.data();
    frame.size = vbuffer_.size();
    frame.is_keyframe = true;
    return true;
}

void AudioVideoFileState::seek_video_to_locked(std::int64_t target_us) {
    // video demuxer independent seek: flush decoder + clear frame to be fetched + seek by time (BACKWARD
    // selects the nearest keyframe at or before the target, then decoding fast-forwards to the target time). Leave the audio cursor unchanged.
    // Note: Only used when EOF is not reached (the seek wraparound of mov demuxer after EOF is unreliable and the loop
    // back to the start of the file uses the decode EOF branch to rebuild everything)
    avcodec_flush_buffers(video_codec_);
    pending_frames_.clear();
    auto target_ts = av_rescale_q(target_us, AVRational{1, 1'000'000},
                                  video_fmt_ctx_->streams[video_stream_]->time_base);
    av_seek_frame(video_fmt_ctx_, video_stream_, target_ts, AVSEEK_FLAG_BACKWARD);
    last_decoded_pts_us_ = -1;
}

void AudioVideoFileState::decode_audio_locked(std::int16_t *dst, std::size_t out_samples) {
    std::size_t got = 0;
    while (got < out_samples) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_INFO("FFmpeg: file ended, looping from start");
                seek_to_start_locked();
                continue;
            }
            break;
        }
        if (packet_->stream_index != audio_stream_) {
            av_packet_unref(packet_);
            continue;
        }
        ret = avcodec_send_packet(audio_codec_, packet_);
        av_packet_unref(packet_);
        if (ret < 0)
            continue;
        while (got < out_samples) {
            ret = avcodec_receive_frame(audio_codec_, aframe_);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                break;
            // swr_convert Output to the remaining position of dst, the number of output samples each time ≤ the number of input frame samples
            std::uint8_t *out_data =
                    reinterpret_cast<std::uint8_t *>(dst + got * channels_);
            int out_count = static_cast<int>(out_samples - got);
            int converted = swr_convert(swr_, &out_data, out_count,
                                        const_cast<const std::uint8_t **>(aframe_->data), aframe_->nb_samples);
            if (converted > 0)
                got += static_cast<std::size_t>(converted);
            av_frame_unref(aframe_);
            if (converted == 0)
                break; // The input sample does not produce output (resampling buffer), waiting for the next frame
        }
    }
    // Fill any shortfall with silence (decoder tail samples/corrupt frames)
    if (got < out_samples)
        std::memset(dst + got * channels_, 0, (out_samples - got) * channels_ * sizeof(std::int16_t));
    // Master clock advance = actual number of output samples
    audio_samples_ += out_samples;
}

void AudioVideoFileState::seek_to_start_locked() {
    // audio loop: audio demuxer + decoder flush + seek back to 0. Main clock (audio_samples_)
    // does not return to zero - it is the playback progress (number of output samples), which must increase monotonically, and the video frames are aligned based on it;
    // Cursor return 0 just re-supplies the input (swr output is a continuous cycle and the content does not jump)
    avcodec_flush_buffers(audio_codec_);
    av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD);
}

std::vector<AudioFormatInfo> AudioVideoFileState::audio_formats() const {
    if (!init_ok_)
        return {{1, 16, 48000}};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: sample_rates_)
        fmts.push_back({channels_, 16, rate});
    return fmts;
}

AudioFormatInfo AudioVideoFileState::audio_current_format() const {
    return {channels_, 16, sample_rate_};
}

bool AudioVideoFileState::audio_set_format(std::uint16_t channels, std::uint8_t bits_per_sample,
                                           std::uint32_t sample_rate) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    if (channels != channels_ || bits_per_sample != 16)
        return false;
    if (sample_rate == sample_rate_)
        return true;
    // only accepts sampling rates in the supported list (resampling targets, audio and video synchronization file sources cannot be arbitrarily changed)
    if (std::find(sample_rates_.begin(), sample_rates_.end(), sample_rate) == sample_rates_.end())
        return false;
    sample_rate_ = sample_rate;
    // reconstruction swresample (target sample rate change)
    swr_free(&swr_);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels_);
    if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, sample_rate_, &audio_codec_->ch_layout,
                            audio_codec_->sample_fmt, audio_codec_->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot recreate swresample for {}Hz", sample_rate);
        return false;
    }
    return true;
}

bool AudioVideoFileState::audio_get_chunk(AudioChunk &chunk) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    // Each block of 1ms data (sampling rate is exactly divisible when it is an integer multiple of 8kHz, aligned with AudioFileSource)
    auto frames_per_chunk = static_cast<std::size_t>(sample_rate_ / 1000);
    chunk_buffer_.resize(frames_per_chunk * channels_ * sizeof(std::int16_t));
    decode_audio_locked(reinterpret_cast<std::int16_t *>(chunk_buffer_.data()), frames_per_chunk);
    chunk.data = chunk_buffer_.data();
    chunk.size = chunk_buffer_.size();
    return true;
}

// ==================== FfmpegVideoSourceView ====================

FfmpegVideoSourceView::FfmpegVideoSourceView(std::shared_ptr<AudioVideoFileState> state) :
    state_(std::move(state)) {
}

std::vector<VideoFormatInfo> FfmpegVideoSourceView::supported_formats() const {
    return state_->video_formats();
}

VideoFormatInfo FfmpegVideoSourceView::current_format() const {
    return state_->video_formats().front();
}

bool FfmpegVideoSourceView::set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                                       std::uint32_t frame_interval) {
    // File source width and height/fixed format: only the format of the file itself is accepted (same verification as FfmpegSource)
    auto fmt = current_format();
    if (fourcc != fmt.fourcc || width != fmt.width || height != fmt.height)
        return false;
    // frame_interval only affects UVC frame descriptor and PROBE negotiation, the file source outputs frames according to the file frame rate
    return true;
}

bool FfmpegVideoSourceView::get_frame(VideoFrame &frame) {
    return state_->video_get_frame(frame);
}

std::size_t FfmpegVideoSourceView::max_frame_size() const {
    return state_->video_max_frame_size();
}

std::uint32_t FfmpegVideoSourceView::frame_interval() const {
    return state_->video_frame_interval();
}

// ==================== FfmpegAudioSourceView ====================

FfmpegAudioSourceView::FfmpegAudioSourceView(std::shared_ptr<AudioVideoFileState> state) :
    state_(std::move(state)) {
}

std::vector<AudioFormatInfo> FfmpegAudioSourceView::supported_formats() const {
    return state_->audio_formats();
}

AudioFormatInfo FfmpegAudioSourceView::current_format() const {
    return state_->audio_current_format();
}

bool FfmpegAudioSourceView::set_format(std::uint16_t channels, std::uint8_t bits_per_sample,
                                       std::uint32_t sample_rate) {
    return state_->audio_set_format(channels, bits_per_sample, sample_rate);
}

bool FfmpegAudioSourceView::get_chunk(AudioChunk &chunk) {
    return state_->audio_get_chunk(chunk);
}

} // namespace usbipdcpp
