#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN // prevents windows.h from pulling in winsock.h (the project uses winsock2, order conflict)
#define NOMINMAX // prevents windows.h from defining min/max macros (projects use std::min/std::max)
#include <windows.h>
#include <mmsystem.h>
// MSVC/clang-cl link winmm(timeBeginPeriod)
#pragma comment(lib, "winmm.lib")
#endif

#include "../example_utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#include "usbipdcpp/virtual_device/video_sources/ColorBarSource.h"
#ifdef USBIPDCPP_USE_FFMPEG
#include "AudioVideoFileSource.h"
#endif
#ifdef USBIPDCPP_USE_MINIAUDIO
#include "AudioFileSource.h"
#endif

using namespace usbipdcpp;

/// Parse a comma-separated list of sampling rates, such as "48000,16000,8000"
static std::vector<std::uint32_t> parse_sample_rates(const std::string &str) {
    std::vector<std::uint32_t> rates;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto rate = std::stoul(item);
        if (rate > 0) {
            rates.push_back(static_cast<std::uint32_t>(rate));
        }
    }
    return rates;
}

/// Parse a harmonic list, e.g. "440:50,880:25:1.5708" (frequency in Hz: amplitude percentage [: phase in radians]; phase is optional
/// Default 0; the amplitude can be negative to indicate inversion, that is, phase flip π).
/// Synthetic formula: y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// Example of the first 3 square-wave harmonics (440Hz, original Fourier coefficient 4/(k*pi), where k is odd):
/// "440:127.324,1320:42.441,2200:25.465" - the default is to divide the whole by the peak value when the superimposed peak value exceeds the full range
/// No clipping (waveform shape remains unchanged), --clamp changes to direct clipping
static std::vector<FourierHarmonic> parse_harmonics(const std::string &str) {
    std::vector<FourierHarmonic> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::stringstream is(item);
        std::string freq_str, amp_str, phase_str;
        if (!std::getline(is, freq_str, ':') || !std::getline(is, amp_str, ':')) {
            continue;
        }
        FourierHarmonic h;
        h.frequency = static_cast<std::uint32_t>(std::stoul(freq_str));
        h.amplitude = std::stod(amp_str) / 100.0;
        if (std::getline(is, phase_str)) {
            h.phase = std::stod(phase_str);
        }
        result.push_back(h);
    }
    return result;
}

/// Create audio source: --audio file source (soft failure output mute when the file cannot be opened, the same as FfmpegSource),
/// --harmonics Fourier series synthesis, otherwise sine wave
static std::unique_ptr<AudioSource> create_source(const cxxopts::ParseResult &result,
                                                  const std::vector<std::uint32_t> &rates,
                                                  const std::vector<FourierHarmonic> &harmonics,
                                                  int channels, int freq, int amp, bool normalize) {
#ifdef USBIPDCPP_USE_MINIAUDIO
    if (result.count("audio") > 0) {
        return std::make_unique<AudioFileSource>(result["audio"].as<std::string>(), rates);
    }
#endif
    if (!harmonics.empty()) {
        return std::make_unique<FourierSource>(harmonics, rates, static_cast<std::uint16_t>(channels), normalize);
    }
    return std::make_unique<SineWaveSource>(static_cast<std::uint32_t>(freq), rates,
                                            static_cast<std::uint16_t>(channels), amp / 100.0);
}

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_webcam", "USB/IP composite device: UVC camera + UAC microphone");
    auto options_group = opts.add_options();
    options_group
        ("width", "Video width", cxxopts::value<int>()->default_value("320"))
        ("height", "Video height", cxxopts::value<int>()->default_value("240"))
        ("fps", "Frame rate", cxxopts::value<int>()->default_value("15"))
        ("freq", "Sine frequency in Hz (integer)", cxxopts::value<int>()->default_value("440"))
        ("rates", "Comma-separated sample rate list in Hz (first is initial)",
         cxxopts::value<std::string>()->default_value("48000"))
        ("channels", "Channel count (1 or 2)", cxxopts::value<int>()->default_value("1"))
        ("amp", "Amplitude 0-100 (percent of full scale)", cxxopts::value<int>()->default_value("50"))
        ("harmonics",
         "Fourier series synthesis: y(t)=sum(A_k*sin(2*pi*f_k*t+phi_k)); harmonics "
         "\"freq:amp[:phase],...\" (freq integer Hz, amp percent of full scale, may be "
         "negative for phase inversion, phase radians default 0; overrides sine). "
         "Square wave example at 440Hz, first 3 harmonics (raw coeffs 4/(k*pi)): "
         "\"440:127.324,1320:42.441,2200:25.465\"",
         cxxopts::value<std::string>())
        ("clamp", "Clip over-limit samples to full scale instead of scaling the whole "
         "wave down by the peak (default: scale only when peak exceeds full scale)",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
#ifdef USBIPDCPP_USE_FFMPEG
    // Only register when FFmpeg is available --video: audio and video synchronization mode (video track → UVC, audio track → UAC,
    // same file, same timeline). In unavailable builds, -h does not display this option, and passing it directly will also be
    // rejected by cxxopts as an unknown option
    options_group("video", "Audio+video file for A/V-synchronized output (requires FFmpeg)",
                  cxxopts::value<std::string>());
#endif
#ifdef USBIPDCPP_USE_MINIAUDIO
    // Only register when miniaudio is available --audio: In unavailable builds -h does not display this option.
    // passing it explicitly is also rejected by cxxopts as an unknown option (parse_example_args prints help and exits)
    options_group("audio", "Audio file to play instead of sine (WAV/MP3/FLAC/OGG)", cxxopts::value<std::string>());
#endif
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto width = result["width"].as<int>();
    auto height = result["height"].as<int>();
    auto fps = result["fps"].as<int>();
    auto freq = result["freq"].as<int>();
    auto channels = result["channels"].as<int>();
    auto amp = result["amp"].as<int>();
    auto clamp = result["clamp"].as<bool>();
#ifdef USBIPDCPP_USE_MINIAUDIO
    auto has_audio = result.count("audio") > 0;
#else
    auto has_audio = false; // --audio not registered (no miniaudio build)
#endif
#ifdef USBIPDCPP_USE_FFMPEG
    auto has_video = result.count("video") > 0;
#else
    auto has_video = false; // --video not registered (no FFmpeg build)
#endif
    auto has_harmonics = result.count("harmonics") > 0;

    // list parameter parsing (stoul/stod throws an exception for illegal input and converts it into a friendly error report to exit)
    std::vector<std::uint32_t> rates;
    std::vector<FourierHarmonic> harmonics;
    try {
        rates = parse_sample_rates(result["rates"].as<std::string>());
        if (has_harmonics) {
            harmonics = parse_harmonics(result["harmonics"].as<std::string>());
        }
    } catch (const std::exception &e) {
        std::cerr << "Failed to parse rates/harmonics: " << e.what() << std::endl;
        return 1;
    }

    if (rates.empty()) {
        std::cerr << "Sample rate list must contain at least one positive rate" << std::endl;
        return 1;
    }
    if (channels != 1 && channels != 2) {
        std::cerr << "Channel count must be 1 or 2" << std::endl;
        return 1;
    }
    if (has_harmonics && harmonics.empty()) {
        std::cerr << "Harmonics list is empty or malformed, use \"freq:amp[:phase],...\"" << std::endl;
        return 1;
    }

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // Video source + audio source: when --video is supplied, use the synchronized audio/video file source (FFmpeg decodes the video track and
    // audio track, audio consumption advances the main clock, and video frames are aligned according to the main clock); otherwise color bar video +
    // synthesized audio (sine/fourier/--audio file)
    std::unique_ptr<VideoSource> video_source;
    std::unique_ptr<AudioSource> source;
    bool av_file_mode = false;
#ifdef USBIPDCPP_USE_FFMPEG
    if (has_video) {
        auto av_state = std::make_shared<AudioVideoFileState>(result["video"].as<std::string>(), rates);
        if (!av_state->ok()) {
            SPDLOG_ERROR("FFmpeg audio/video source initialization failed; exiting");
            return 1;
        }
        video_source = std::make_unique<FfmpegVideoSourceView>(av_state);
        source = std::make_unique<FfmpegAudioSourceView>(av_state);
        av_file_mode = true;
    } else
#endif
    {
        video_source = std::make_unique<ColorBarSource>(width, height, fps);
        source = create_source(result, rates, harmonics, channels, freq, amp, !clamp);
    }
    // The number of channels is subject to the actual sound source.
    channels = source->current_format().channels;

    // high-speed isochronous endpoint: bInterval=4 → one packet per 1ms (aligned kernel gadget f_uac1.c’s as_in_ep_desc,
    // real UAC device form), the endpoint size is reserved according to the highest sampling rate frame alignment.
    // packet scheduling (aligned kernel gadget u_audio.c) is "base packet length + residual overflow plus one frame", and the overflow packet is one frame larger than the base.
    // So wMaxPacketSize must be rounded up by frame (frame size × ceil(max_rate/1000)): non-1kHz integer multiples of the sampling rate
    // Rounding by byte under  (such as 44100 mono = 88.2 bytes/ms → 88/90 byte packet alternation) will truncate the overflow packet and chop the frame.
    // must use the maximum sampling rate of the format actually supported by the sound source: the soft failure downgrade format (48000) may not be in the command line rates.
    // endpoint will be too small according to the command line, and the isochronous data will be truncated.
    auto fmts = source->supported_formats();
    auto max_rate = std::max_element(fmts.begin(), fmts.end(),
                                     [](const AudioFormatInfo &a, const AudioFormatInfo &b) {
                                         return a.sample_rate < b.sample_rate;
                                     })->sample_rate;
    auto iso_packet_size = static_cast<std::uint16_t>(((max_rate + 999) / 1000) * 2 * channels);

    // interface 0-1: UVC (VC + VS), interface 2-3: UAC microphone (AC + AS).
    // UAC AS uses 0x83 rather than 0x81 for ISO IN: endpoint numbers are unique within the device, and UVC VS already uses 0x81
    // (aligned core g_webcam: ep1 for uvc, ep2/3 for uac)
    std::vector<UsbInterface> interfaces = {
            // Interface 0: VideoControl (including interrupt endpoint for status notification)
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOCONTROL,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x87, // IN, endpoint 7 — interrupt for status
                            .attributes = 0x03, // Interrupt
                            .max_packet_size = 16,
                            .interval = 8,
                    }}},
            },
            // Interface 1: VideoStreaming (alt 0 has no endpoints, alt 1 ISO IN endpoint)
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOSTREAMING,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x81, // IN, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = 512,
                                          .interval = 1,
                                  }}},
            },
            // Interface 2: AudioControl (including interrupt endpoint for status notification,
            // aligns the core gadget ac_int_ep_desc of f_uac1.c: wMaxPacketSize=2, bInterval=4)
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x82, // IN, endpoint 2 — interrupt for status
                            .attributes = 0x03, // Interrupt
                            .max_packet_size = 2, // UAC1 status word (bStatusType + bOriginator)
                            .interval = 4,
                    }}},
            },
            // Interface 3: AudioStreaming (alt 0 has no endpoints, alt 1 ISO IN endpoint)
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x83, // IN, endpoint 3 (0x81 is occupied by UVC VS)
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = iso_packet_size,
                                          .interval = 4, // One packet per 1ms (aligned gadget f_uac1.c)
                                  }}},
            },
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_webcam",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5685,
            .device_bcd = 0x0100,
            .device_class = 0xEF, // Miscellaneous (IAD)
            .device_subclass = 0x02, // Common Class
            .device_protocol = 0x01, // Interface Association Descriptor
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });

    // interface is numbered consecutively from 0: fill in interface_number in sequence according to the subscript (bInterfaceNumber/IAD depends on it)
    device->assign_interface_numbers();
    // UVC first, then UAC: UAC helper will open the device-level scheduler (isochronous delayed response according to frame rhythm),
    // UVC isochronous URB does not go through the scheduler (response immediately, see UvcDeviceHelper::setup comment) and does not affect each other
    if (auto ec = UvcDeviceHelper::setup(device, 0, string_pool, std::move(video_source)); ec) {
        SPDLOG_ERROR("UVC function assembly failed: {}", ec.message());
        return 1;
    }
    if (auto ec = UacDeviceHelper::setup_microphone(device, 2, string_pool, std::move(source)); ec) {
        SPDLOG_ERROR("UAC function assembly failed: {}", ec.message());
        return 1;
    }

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    std::string rates_str;
    for (auto r: rates)
        rates_str += std::to_string(r) + " ";
    auto mode_str = av_file_mode ? std::string("AV file (A/V synchronized)")
                                 : (has_audio ? std::string("audio file")
                                              : (has_harmonics ? std::string("fourier harmonics")
                                                               : std::to_string(freq) + "Hz sine"));
    if (av_file_mode) {
        SPDLOG_INFO("Mock webcam started on port {}, busid {}, video from {}, mic {}, {}ch, rates [{}]", port, busid,
                    result["video"].as<std::string>(), mode_str, channels, rates_str);
    } else {
        SPDLOG_INFO("Mock webcam started on port {}, busid {}, {}x{}@{}fps + mic {}, {}ch, rates [{}]", port, busid,
                    width, height, fps, mode_str, channels, rates_str);
    }
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    server.stop();
    return 0;
}
