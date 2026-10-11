#include "PlaybackSink.h"

#include <cstring>

// single header library: only this compilation unit provides implementation
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

PlaybackSink::PlaybackSink(std::string device_name) : device_name(std::move(device_name)) {
    device = new ma_device{};
    open_device();
}

PlaybackSink::~PlaybackSink() {
    close_device();
    delete static_cast<ma_device *>(device);
}

std::vector<AudioFormatInfo> PlaybackSink::supported_formats() const {
    // common sampling rate × single and dual channels (16 bit PCM), resampling is left to the sound card driver
    static const std::uint32_t rates[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: rates) {
        fmts.push_back({1, 16, rate});
        fmts.push_back({2, 16, rate});
    }
    return fmts;
}

AudioFormatInfo PlaybackSink::current_format() const {
    std::lock_guard lock(mutex);
    return format;
}

bool PlaybackSink::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    std::unique_lock lock(mutex);
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        return false;
    }
    bool restart = !discarding && (channels != format.channels || sample_rate != format.sample_rate);
    format = {channels, bits_per_sample, sample_rate};
    // Clear the residual buffer when switching the sampling rate: the PCM of the old sampling rate does not match the new format,
    // and residual buffer occupancy makes the incoming-stream rate-control loop act in the wrong direction for a long time (observed after switching:
    // hundreds of thousands of bytes remain buffered; the control loop keeps delaying responses, causing underrun noise)
    buffer.clear();
    lock.unlock();
    if (restart) {
        // cannot restart while holding the lock: ma_device_uninit Wait for the callback thread to exit, and the callback will deadlock if it takes the same lock.
        open_device();
    }
    return true;
}

void PlaybackSink::write_pcm(const std::uint8_t *data, std::size_t size) {
    std::lock_guard lock(mutex);
    received += size;
    if (discarding) {
        return;
    }
    // New data is lost when the buffer is full (RingBuffer.write returns the actual number of bytes written), and the callback side is silent when filling in missing data
    auto written = buffer.write(data, size);
    if (written < size) {
        SPDLOG_WARN("PlaybackSink: Buffer full, discarding {} bytes", size - written);
    }
}

void PlaybackSink::reset() {
    std::lock_guard lock(mutex);
    buffer.clear();
}

std::uint64_t PlaybackSink::received_bytes() const {
    std::lock_guard lock(mutex);
    return received;
}

void PlaybackSink::open_device() {
    close_device();
    discarding = false;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = format.channels;
    config.sampleRate = format.sample_rate;
    config.dataCallback = data_callback;
    config.pUserData = this;
    // Android's AAudio/OpenSL default buffer is too large (hundreds of ms, measured half-second delay),
    // explicitly sets a small callback period (20ms) to suppress the playback delay; jitter is absorbed by the device side RingBuffer
    config.periodSizeInMilliseconds = 20;

    ma_result result;
    if (device_name.empty()) {
        result = ma_device_init(NULL, &config, static_cast<ma_device *>(device));
    }
    else {
        // Specify the device name: self-created context enumeration device matching name. Note the ma_device_init post-context
        // must remain alive, so the members are saved; they are released together with the device when destructed.
        auto *ctx = new ma_context;
        context = ctx;
        if (ma_context_init(NULL, 0, NULL, ctx) != MA_SUCCESS) {
            SPDLOG_ERROR("PlaybackSink: miniaudio context initialization failed and entered discard mode");
            discarding = true;
            return;
        }
        ma_device_info *devices = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(ctx, &devices, &count, NULL, NULL) != MA_SUCCESS || count == 0) {
            SPDLOG_ERROR("PlaybackSink: Failed to enumerate playback devices and entered discard mode");
            discarding = true;
            return;
        }
        bool found = false;
        for (ma_uint32 i = 0; i < count; i++) {
            if (device_name == devices[i].name) {
                config.playback.pDeviceID = &devices[i].id;
                found = true;
                break;
            }
        }
        if (!found) {
            // Lists the available device names line by line when an error is reported, allowing users to specify the correct --device
            SPDLOG_ERROR("PlaybackSink: Playback device not found \"{}\", available device list:", device_name);
            for (ma_uint32 i = 0; i < count; i++) {
                SPDLOG_INFO("  - \"{}\"", devices[i].name);
            }
            SPDLOG_ERROR("Entering discard mode");
            discarding = true;
            return;
        }
        result = ma_device_init(ctx, &config, static_cast<ma_device *>(device));
    }

    if (result != MA_SUCCESS) {
        SPDLOG_ERROR("PlaybackSink: Failed to open the playback device ({}) and entered discard mode", ma_result_description(result));
        discarding = true;
        return;
    }
    device_initialized = true;
    if (ma_device_start(static_cast<ma_device *>(device)) != MA_SUCCESS) {
        SPDLOG_ERROR("PlaybackSink: Failed to start playback and entered discard mode");
        close_device();
        discarding = true;
        return;
    }
    // default device (not specified --device): print the actual device name used - ma_device initialization
    // has completed, pContext is valid; query device information using playback.id
    if (device_name.empty()) {
        auto *dev = static_cast<ma_device *>(device);
        ma_device_info info;
        if (ma_context_get_device_info(dev->pContext, ma_device_type_playback, &dev->playback.id, &info) ==
            MA_SUCCESS) {
            SPDLOG_INFO("PlaybackSink: Use default playback device \"{}\"", info.name);
        }
    }
    auto *dev = static_cast<ma_device *>(device);
    SPDLOG_INFO("PlaybackSink: Start playback (request {}ch {}Hz, device actual sampleRate={} channels={})",
                format.channels, format.sample_rate, dev->sampleRate, dev->playback.channels);
}

void PlaybackSink::close_device() {
    auto *dev = static_cast<ma_device *>(device);
    if (device_initialized) {
        // uninit will wait for the callback thread to exit before returning
        ma_device_uninit(dev);
        device_initialized = false;
    }
    auto *ctx = static_cast<ma_context *>(context);
    if (ctx != nullptr) {
        ma_context_uninit(ctx);
        delete ctx;
        context = nullptr;
    }
}

void PlaybackSink::data_callback(struct ma_device *device, void *output, const void *input,
                                 std::uint32_t frame_count) {
    auto *self = static_cast<PlaybackSink *>(device->pUserData);
    auto frame_size = ma_get_bytes_per_frame(device->playback.format, device->playback.channels);
    auto n = static_cast<std::size_t>(frame_count) * frame_size;
    std::lock_guard lock(self->mutex);
    auto read = self->buffer.read(static_cast<std::uint8_t *>(output), n);
    if (read < n) {
        std::memset(static_cast<std::uint8_t *>(output) + read, 0, n - read); // Missing data, fill in mute
    }
}

} // namespace usbipdcpp
