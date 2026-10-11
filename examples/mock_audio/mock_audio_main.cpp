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
#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
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

/// Analyze harmonic list, such as "440:50,880:25:1.5708" (frequency Hz: amplitude percentage [: phase radian], phase can be omitted
/// Default 0; the amplitude can be negative to indicate inversion, that is, phase flip π).
/// Synthetic formula: y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// Example using the first 3 square-wave harmonics (440Hz, original Fourier coefficient 4/(kπ), k is an odd number):
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
    auto opts = make_example_options("mock_audio", "USB/IP virtual UAC microphone");
    auto options_group = opts.add_options();
    options_group
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
#ifdef USBIPDCPP_USE_MINIAUDIO
    // Only register when miniaudio is available --audio: In unavailable builds -h does not display this option.
    // passing it explicitly is also rejected by cxxopts as an unknown option (parse_example_args prints help to exit)
    options_group("audio", "Audio file to play instead of sine (WAV/MP3/FLAC/OGG)", cxxopts::value<std::string>());
#endif
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto freq = result["freq"].as<int>();
    auto channels = result["channels"].as<int>();
    auto amp = result["amp"].as<int>();
    auto clamp = result["clamp"].as<bool>();
#ifdef USBIPDCPP_USE_MINIAUDIO
    auto has_audio = result.count("audio") > 0;
#else
    auto has_audio = false; // --audio not registered (no miniaudio build)
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

    // Audio source: --audio uses the file audio source when provided (the number of channels is determined by the file, --channels is ignored);
    // --harmonics uses Fourier series synthesis when provided; otherwise it is a sine wave.
    // When the file cannot be opened, the AudioFileSource soft fails (log error + output mute), but the device can still be enumerated (same as FfmpegSource)
    auto source = create_source(result, rates, harmonics, channels, freq, amp, !clamp);
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

    std::vector<UsbInterface> interfaces = {
            // Interface 0: AudioControl (including interrupt endpoint for status notification,
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
            // Interface 1: AudioStreaming (alt 0 has no endpoints, alt 1 ISO IN endpoint)
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x81, // IN, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = iso_packet_size,
                                          .interval = 4, // One packet per 1ms (aligned gadget f_uac1.c)
                                  }}},
            },
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_audio",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5682,
            .device_bcd = 0x0100,
            .device_class = 0x00, // single-function device, interfaces are defined individually (no IAD required)
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });

    // UacDeviceHelper creates AC/AS handler and registers + sets descriptor.
    // interface is numbered consecutively from 0: fill in interface_number in sequence according to the subscript (bInterfaceNumber depends on it)
    device->assign_interface_numbers();
    if (auto ec = UacDeviceHelper::setup_microphone(device, 0, string_pool, std::move(source)); ec) {
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
    auto mode_str = has_audio ? std::string("audio file")
                              : (has_harmonics ? std::string("fourier harmonics")
                                               : std::to_string(freq) + "Hz sine");
    SPDLOG_INFO("Mock UAC microphone started on port {}, busid {}, {}, {}ch, rates [{}]", port, busid, mode_str,
                channels, rates_str);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    server.stop();
    return 0;
}
