# usbipdcpp

A C++ library for creating usbip servers

> [English Documentation](README.md)

## Features

- ✅ **USB/IP server**: Implemented with libusb, supports all libusb-compatible platforms
- ✅ **Four USB transfer types** (control, bulk, interrupt, isochronous) all passed the libusb backend test
- ✅ **Virtual devices**: HID (mouse, keyboard, gamepad, touchscreen), MSC (USB flash drive), CDC ACM (serial port), CDC ECM/RNDIS (network adapter), UVC (camera), UAC (microphone, speaker), webcam (composite UVC camera + UAC microphone device) — no libusb required
- 🔌 **Hotplug support**: Automatically detects device insertion/removal (LibusbServer)
- 🧩 **Extensible design**: Provides a complete abstract interface for developers to extend

Code contributions are welcome! 🚀

> 💡 **Tip**: If this project is useful to you, please consider giving it a ⭐, which can help more people discover it.

---

## Quick Start

> ⚠️ **Version Tips**: This library is still in its early stages, and the API may change between different minor versions (strict semantic version compatibility is not guaranteed yet).
> It is recommended to pin a specific version in your project (for example, the latest release when you start using it) and review the commit history for changes before upgrading.

The simplest virtual keyboard device (see `examples/mock_keyboard` for a complete example):

```cpp
#include <cstdint>
#include <iostream>
#include <vector>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

int main() {
    StringPool string_pool;

    // 1. Use KeyboardHandler::make_interface to build HID keyboard interface descriptor template
    // (class/subclass/protocol + interrupt IN endpoint at the given address); make_interface does not bind a handler,
    // Binding must happen after device creation (the interface is stored in the device and its address is stable; see below)
    // 2. Create device from interface list - UsbDevice::make provides reasonable defaults for remaining fields
    //    (speed=Full, EP0 automatically generated from speed, etc.); bind interface handlers before the device-level handler
    auto device = UsbDevice::make("1-1", 0x1234, 0x5679,
                                  {KeyboardHandler::make_interface(0x81)});
    device->interfaces[0].with_handler<KeyboardHandler>(string_pool);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // 3. Start USB/IP server (TCP, listen on 53240 port)
    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint ep{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(ep); ec) {
        std::cerr << "Server startup failed: " << ec.message() << std::endl;
        return 1;
    }
    // Client connection: usbip attach -r <host> -b 1-1
    // After connection, KeyboardHandler can be driven in any thread.
    // For example kb->press_key(HIDKey::A) / kb->release_key(HIDKey::A)

    std::cin.get(); // Press Enter to exit
    server.stop();
}
```

This library supports two types of devices: **virtual devices** (pure software, as shown above - see `examples/mock_keyboard`, `mock_mouse`, `mock_msc`, etc.) and **libusb devices** (physical USB devices shared over the network - see `examples/libusb_server`).

---

## Compile and install

### Precompiled binary

Every time the version tag is pushed, the precompiled packages for each platform will be released to the [Releases](https://github.com/yunsmall/usbipdcpp/releases) page.

If you want to get the package without waiting for the release, or want to build from a specific commit:

1. **Fork** this repository
2. Select **"Build packages (manual)"** → **"Run workflow"** in the **Actions** tab of the fork repository
3. After the workflow completes, download the build artifact for your platform

> ⚠️ The `linux-aarch64` task requires the ARM64 runner, which GitHub provides for free only to **public** repositories.
> If your fork is private, this task will be skipped and the remaining three platforms will still build normally.

### Compiler requirements

If compiled with gcc, the minimum gcc version is **gcc13**. Although gcc13 supports C++23,
`std::println` is only supported from gcc14, and only `std::format` can be used under gcc13.
In order to be compatible with gcc13, this library code uses `std::format` instead of `std::println`.

### CMake options

There are multiple CMake options used to control whether the corresponding module is compiled:

| Options | Default | Description |
|------|--------|------|
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | Compile server components based on libusb |
| `USBIPDCPP_BUILD_VIRTUAL_DEVICE` | ON | Compile virtual device components |
| `USBIPDCPP_BUILD_SHARED_LIBS` | ON | Compile as a dynamic library (in line with LGPL compliance requirements, recommended); set to OFF to compile a static library |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (top-level project) | Compile all sample programs |
| `USBIPDCPP_BUILD_TESTS` | ON (top-level project) | Compile test suite |
| `USBIPDCPP_USE_PKGCONF_ASIO` | OFF | Use pkgconf to find asio instead of CMake's `find_package`. asio only provides `asio.pc` and must be enabled when there is no CMake config (this is the case for `libasio-dev` installed by apt and `libasio` by Termux); asio of vcpkg comes with CMake config, so this is not required |

For more options, see `CMakeLists.txt`

### Complete compilation command:

#### Linux (Ubuntu/Debian) — does not use vcpkg

Install dependencies directly through apt without vcpkg:

```bash
# required
sudo apt install libasio-dev libspdlog-dev

# If you need to compile the example (default USBIPDCPP_BUILD_EXAMPLES=ON)
sudo apt install libcxxopts-dev

# If you need to compile and test (default USBIPDCPP_BUILD_TESTS=ON)
sudo apt install libgtest-dev

# If you need libusb forwarding of physical devices (default USBIPDCPP_BUILD_LIBUSB_COMPONENTS=ON)
sudo apt install libusb-1.0-0-dev

# If you need --audio audio file playback of mock_audio / local playback of mock_speaker (optional, located in the universe software source; automatically skipped if not installed)
sudo apt install libminiaudio-dev

# Compile
# apt's libasio-dev provides only asio.pc, without CMake config, so enable USBIPDCPP_USE_PKGCONF_ASIO
cmake -B build -DUSBIPDCPP_USE_PKGCONF_ASIO=ON
cmake --build build
cmake --install build
```

When disabling a feature with a CMake option, its corresponding apt package can be omitted:
- `-DUSBIPDCPP_BUILD_EXAMPLES=OFF` → No need to install `libcxxopts-dev`
- `-DUSBIPDCPP_BUILD_TESTS=OFF` → No need to install `libgtest-dev`
- `-DUSBIPDCPP_BUILD_LIBUSB_COMPONENTS=OFF` → No need to install `libusb-1.0-0-dev`

If there is no `libminiaudio-dev` in your software source, you can download it from the official website of [miniaudio](https://miniaudio.app/)
Download `miniaudio.h` and place it in the include path. If the miniaudio header cannot be found,
the mock_audio `--audio` option (`AudioFileSource`) is automatically skipped (a WARNING is emitted during configuration).
Other audio sources are not affected. miniaudio supports two search methods: directly search for the header file, or use pkgconf if it cannot be found.
The source code adapts to the differing stb_vorbis layouts (vcpkg: include root; Termux: include/stb/) through
`__has_include`; if stb is absent, only OGG decoding is unavailable (WAV/MP3/FLAC work normally).

#### Windows — vcpkg

Install [Visual Studio](https://visualstudio.microsoft.com/) (select the "Desktop development with C++" workload) and [vcpkg](https://github.com/microsoft/vcpkg).

Install dependencies:

```bash
./vcpkg install asio libusb spdlog cxxopts gtest miniaudio
```

Configure and compile (do not specify a generator, use the Visual Studio generator and MSVC by default):

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
cmake --install build
```

Notes:

- The Visual Studio generator supports multiple configurations: add `--config Release` (or Debug) to `cmake --build` and `ctest`.
- The dependent library under Windows is a DLL. `cmake --install` will automatically copy the dependent DLL next to the executable file.
- Skip the corresponding vcpkg packages when disabling their features: tests → `gtest`, examples → `cxxopts`, libusb components → `libusb`, mock_audio audio-file playback / mock_speaker local playback → `miniaudio`.

#### Termux (Android)

Termux is compiled using clang and is not restricted by the gcc13 version above.

Install dependencies:

```bash
pkg update
pkg install clang cmake ninja pkg-config libasio libspdlog googletest libusb libc++
```

There is no `cxxopts` in the Termux repository. If you need to compile the example, please install it manually (the following uses installation to `$PREFIX/opt/cxxopts` as an example):

```bash
git clone https://github.com/jarro2783/cxxopts.git
cd cxxopts
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX/opt/cxxopts
cmake --build build
cmake --install build
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja \
    -DUSBIPDCPP_USE_PKGCONF_ASIO=ON \
    -Dcxxopts_DIR=$PREFIX/opt/cxxopts/share/cmake/cxxopts
cmake --build build
cmake --install build --prefix $PREFIX
```

Notes:

- `-DUSBIPDCPP_USE_PKGCONF_ASIO=ON` must be turned on: Termux's libasio is built with autotools and only provides `asio.pc` without CMake config.
- Explicitly install `libc++` among the dependencies: libc++ headers come from `ndk-sysroot`, while its runtime library (`libc++_shared.so`) comes from the `libc++` package. If the former is upgraded to the repository version without the latter, their versions become mismatched, causing an undefined `std::__ndk1::__hash_memory` error at link time. Explicit installation keeps both at the same version.
- It is also possible not to install cxxopts, the examples will be skipped entirely (there will be a WARNING when configure); you can also compile only the library through `-DUSBIPDCPP_BUILD_EXAMPLES=OFF -DUSBIPDCPP_BUILD_TESTS=OFF`.
- `libevdev_mouse` and `mock_uvc_ffmpeg` depend on libevdev/FFmpeg, which does not have a dev package in Termux and is automatically skipped during configure without additional options.
- There is no miniaudio in the Termux repository, and the `--audio` option (`AudioFileSource`) of mock_audio and the native playback of mock_speaker are automatically skipped; the header file can be manually installed to enable it. The stb header file is placed in the `include/stb/` subdirectory in Termux (vcpkg is in the include root directory), and the source code uses `__has_include` to adapt to the two layouts.
- To compile the termux_libusb_server example, add `-DUSBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER=ON`; running it requires `pkg install termux-api` (provides the termux-usb command).

#### Using vcpkg package manager
Please install asio libusb libevdev spdlog and other libraries in advance.
If you want to compile the example, you also need to install cxxopts:
```bash
./vcpkg install asio libusb libevdev spdlog cxxopts
```
If you need to compile the tests, also install gtest:
```bash
./vcpkg install gtest
```
If you want to play the audio files of mock_audio (`AudioFileSource`) or the local playback of mock_speaker, you also need to install miniaudio:
```bash
./vcpkg install miniaudio
```

```bash
cmake -B build \
-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
cmake --install build
```

#### Use conan as the package manager:
```bash
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build build/Release
cmake --install build/Release
```

### Run tests

After turning on test compilation (top-level projects are enabled by default), run:

```bash
ctest --test-dir build --output-on-failure
```

network test suite covers:

- Server restart cycles (`start` → `stop` → `start`), including 100 cycles and concurrent races between stop and connections
- Client disconnection at various protocol stages: idle period (no data sent), half request received, garbage data, immediate disconnection after import, URB transfer disconnection - two methods: graceful disconnection (FIN) and abnormal disconnection (RST)
- Device life cycle: repeated import/release, multiple clients seizing the same device, importing non-existent devices

### Python bindings

Python binding uses pybind11, you need to install `pybind11` first (either vcpkg or pip).

**Compile:**
```bash
cmake -B build -DUSBIPDCPP_BUILD_PYTHON_BINDINGS=ON
cmake --build build
```

After building, set PYTHONPATH and test:
```bash
# Windows
set PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
# Linux/macOS
export PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
```

**Dependencies:**
- pybind11 (vcpkg installation: `./vcpkg install pybind11`, or pip: `pip install pybind11`)
- Optional, used to generate `.pyi` stub file: `pip install pybind11-stubgen`

**Note:**
- `send_input_report()` accepts `bytes`: `handler.send_input_report(b'\x01\x00\x00\x00')`
- Python can inherit `HidVirtualInterfaceHandler` to implement custom HID devices (reference `examples/python/flip_left_button.py`)
- `start()` and `stop()` will release the Python GIL and can be safely called in the main thread
- Python can call `Session.immediately_stop()`; under normal circumstances, this interface is only used internally by the server stop process.

### Python binding development status 🚧

The Python bindings are **under active development** and may contain bugs or cause crashes.

If you encounter an error:

1. **Report an issue**, attach a complete Python traceback (traceback)
2. **Get C++ stack**: Debugging native applications in CLion
   - Run → Edit Configurations → Add New → **Native Application**
   - Target: Select `usbipdcpp_python`
   - Executable: Select `python.exe` path
   - Program arguments: target script path (such as `examples/python/flip_left_button.py`)
   - Working directory: project root directory
   
   CLion will stop on crash and display the complete C++ call stack.

3. Please provide:
   - Python version and operating system
   - Build configuration used
   - Full error output (Python traceback + C++ stack)
   - Minimal reproducible code

---

## Use

```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# Or you want to use the libusb function

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::libusb)
```

### Extended functions

A complete guide to implementing custom devices (including two implementation methods and complete code examples) is available at [docs/custom-device.md](docs/custom-device.md).

Implement a custom USB device:

1. Use `usbipdcpp::UsbDevice` to define the device descriptor
2. Inherit `AbstDeviceHandler` to implement device logic
3. Use `VirtualInterfaceHandler` to process interface operations and implement the endpoint logic within the interface.

Use each handler's `make_interface` factory to define interfaces: it creates an interface matching the handler's expected
endpoint structure (a handwritten `UsbInterface` may conflict with hardcoded assumptions in the interface handler).
The factory returns only a descriptor template, without binding a handler: the interface handler constructor retains an interface reference, so bind it after device creation
is complete (the interface is in `device->interfaces` and its address is stable) using `device->interfaces[i].with_handler<T>(...)`.
`UsbDevice` itself can be completely constructed manually to facilitate more flexible configuration.

Simple devices can directly use `SimpleVirtualDeviceHandler`, which provides an empty implementation for standard requests

> ⚠️ **IMPORTANT NOTE**: When overriding `VirtualInterfaceHandler::on_new_connection()` and `on_disconnection()`, you **must** call the parent class implementation. The parent class sets/clears the `session` pointer, which is used to submit response data.

### Custom USB string

Call `change_string_*` **before** starting the server to modify device strings:

**Device Level String** (via `VirtualDeviceHandler`):
```cpp
device_handler->change_string_manufacturer(L"My Company");
device_handler->change_string_product(L"My USB Device");
device_handler->change_string_serial(L"1234567890");
device_handler->change_string_configuration(L"My Configuration");
```

**Interface String** (via `VirtualInterfaceHandler`):
```cpp
interface_handler->change_string_interface(L"My HID Interface");
```

All `change_string_*` methods eventually call `StringPool::change_string()`. If the string index is invalid, an exception will be thrown.

---

## Example

**1. libevdev_mouse**

   uses the libevdev library to simulate a usbip mouse by reading `/dev/input/event*` on a system that supports evdev to forward local mouse signals.
**2. mock_mouse**

   A relative mouse example based on `RelativeMouseHandler`. By default, the left button state is switched every second.
   plus `--circle` parameter controls the cursor movement in circles.
**3. mock_keyboard**

   A keyboard example, based on the `KeyboardHandler` class, with built-in Consumer Control (media keys such as volume, playback and pause).
   Simulates pressing and releasing the 'A' key every second.
**4. mock_gamepad**

   A gamepad example based on `GamepadHandler`. The D-pad rotates through eight directions, the left stick traces a circle, and button 0 alternates on/off.
**5. mock_cdc_acm**

   virtual serial port (CDC ACM) example, showing bidirectional data transfer of USB Bulk endpoint.
**6. mock_cdc_throttle**

   Rate-limited virtual serial port (CDC ACM) example, demonstrating **OUT NAK backpressure**: within a fixed window of `--window-sec` seconds, receive at most
   `--limit-bytes` bytes; once the quota is exhausted, stop receiving (OUT NAK: host URBs remain pending), then resume and drain
   pending requests when the window ends. The data processor also counts received `'1'` characters, returning the cumulative count to the host every half-window
   (`<number>\n`). Use: `mock_cdc_throttle -l 64 -w 5`.
**7. multi_devices**

   An example containing 10 virtual HID devices, demonstrating how to create multiple devices with the factory pattern.
**8. absolute_mouse**

   Absolute coordinate mouse virtual device example, providing a complete mouse operation API:
   - **Screen coordinate API**: Use pixel coordinate positioning and set the screen boundary through `set_screen_bounds()`
   - **Raw HID coordinate API**: Methods with the `_raw` suffix operate directly on HID coordinates (0-32767)
   - **Movement function**: `move(from, to)` and `humanized_move(from, to)` accept start and end parameters
   - **Drag function**: `drag(from, to)` and `humanized_drag(from, to)` press the left button to move
   - **Button operation**: left button, right button, middle button, click, double click
   
   `set_screen_bounds(x1, y1, x2, y2)` Working principle:
   - defines the valid range boundary of screen coordinates, such as `bounds(0, 0, 1920, 1080)` represents the screen range [0, 1920] × [0, 1080]
   - Screen coordinates are converted by a linear mapping into HID coordinates [0, 32767]
   - Coordinates beyond bounds will be clamped to the boundary value
   - Note: Windows hosts do not accept HID (0, 0) and it is recommended that screen coordinates avoid (x1, y1) boundaries
**9. libusb_server**

   forwards the local USB device with a very simple command line. Enter `h` to view the usage, and you can choose which devices to forward.
   Shares the same usbip server with virtual devices: `LibusbServer` contains a `Server`;
   physical devices enter its device table through bind, and virtual devices are added with `get_server().add_device(virtual device)` to
   the same Server. One listening port exports both device types (Server is unaware of the device backend).
   Ensure busid uniqueness when mixing them: clients attach by busid, and a virtual device's default busid often conflicts with a physical
   device's bus number (such as "1-1"); on conflict, `add_device` rejects insertion (returns `AlreadyBound`),
   so assign the virtual device a separate, nonconflicting busid
**10. mock_msc**

   is a virtual USB mass storage (USB flash drive) device that uses disk image files as storage media.
   supports BOT (Bulk-Only Transport) protocol and common SCSI commands (INQUIRY, READ CAPACITY,
   READ(10), WRITE(10), MODE SENSE, etc.). The underlying storage can be replaced through the `StorageBackend` abstract interface
   - The example uses `RawImageBackend` to implement it as a memory mapped file. It can also be polymorphically connected to custom backends such as qcow2.

   uses: `mock_msc [disk.img]` (default `disk.img`, 4096 blocks × 512 bytes = 2 MiB)

   **Enable discard/TRIM (hole punching)**:

   The Linux kernel skips VPD query for USB storage devices by default, causing the UNMAP command to fail to be issued. Need to be enabled on the client:

   ```bash
   # View device SCSI path
   ls /sys/class/scsi_disk/
   # Write unmap to the found path (replace X:0:0:0 with the actual value)
   echo unmap > /sys/class/scsi_disk/X:0:0:0/provisioning_mode
   ```

   or create udev rules to take effect permanently:
   ```bash
   # /etc/udev/rules.d/50-usb-ssd-trim.rules
   ACTION=="add|change", SUBSYSTEM=="scsi_disk", ATTR{provisioning_mode}="unmap"
   ```

   After enabling, manually trigger discard with `sudo fstrim -v /mountpoint`,
   or enable scheduled trim with `sudo systemctl enable fstrim.timer`.

**11. mock_uvc**

   virtual UVC camera, use `ColorBarSource` to output 320×240 YUY2 color bar test pattern.
   demonstrates how to implement the `UvcVideoControlHandler` + `UvcVideoStreamingHandler` + `VideoSource` combination.
   is available for both Linux and Windows.

**12. mock_uvc_ffmpeg**

   virtual UVC camera, reads video files as video source through FFmpeg. Supports all formats that FFmpeg can decode
   (MP4, MKV, AVI, etc.), with optional MJPEG/H264 passthrough mode.

   Usage: `mock_uvc_ffmpeg --video video.mp4` (add `--passthrough` to enable MJPEG/H264 passthrough)

   requires FFmpeg libraries (libavformat, libavcodec, libswscale, libavutil).

**13. mock_audio**

   Virtual USB Microphone (UAC 1.0). Demo
   `UacAudioControlHandler` + `UacAudioStreamingSourceHandler` + `AudioSource` combination
   (Feature Unit mute/volume control, sample rate negotiation, ISO PCM push).

   supports three audio sources:
   - Sine wave test tone (default): `--freq 440 --amp 50`
   - Fourier series synthesis: `--harmonics "440:50,880:25"` (frequency Hz: amplitude % [: phase rad])
   - Audio file playback: `--audio music.mp3` (WAV/MP3/FLAC/OGG, based on miniaudio, default loop playback;
     requires miniaudio to be installed, automatically skipped if not found)

   Use: `mock_audio --rates 48000,16000,8000` (the first one is the initial sampling rate)

**14. mock_speaker**

   Virtual USB speaker (UAC 1.0, ISO OUT receive-stream direction). Demonstrates the
   `UacAudioControlHandler` + `UacAudioStreamingSinkHandler` + `AudioSink` combination
   (Feature Unit mute/volume control, sample-rate negotiation, reception and consumption of ISO OUT PCM streams).

   Three consumption methods:
   - local playback (default): based on miniaudio, miniaudio needs to be installed, automatically skipped if not found
   - Save WAV to disk: `--output out.wav` (write received PCM to a WAV file, no miniaudio required)
   - Discard and count (no miniaudio and no `--output`): only count received bytes

   uses: `mock_speaker --rates 48000,44100,96000 --channels 2` (the first one is the initial sampling rate;
   specify playback device with `--device <name>`; defaults to the system's default device)

**15. termux_libusb_server**

   libusb server that can be used in termux on non-rooted Android devices, via
   `termux-usb -e /path/to/termux_libusb_server /dev/bus/usb/xxx/xxx` starts.

   Since termux-usb passes only one fd, start multiple servers on different ports to support multiple devices.
   Use the `USBIPDCPP_LISTEN_PORT` environment variable to specify the listening port

   For the use of  termux-usb, please check the official termux related documents.

**16. multi_interface_hid**

   Composite USB device example, implementing **two HID interfaces** (mouse + keyboard) simultaneously on a single device.
   shows how to use `SimpleVirtualDeviceHandler` to create a multi-interface virtual device.

**17. mock_pipe**

   Universal virtual pipe device (vendor class interface, bulk IN + bulk OUT). Showing `PipeDeviceHandler`
   read()/write() interface: operate the endpoint data stream of the virtual device like reading and writing files (FunctionFS style
   FIFO blocking semantics; non-standard control requests are also returned via read() in PipeXfer with setup_req).

**18. mock_pipe_hid**

   A **HID keyboard** implemented using generic `PipeDeviceHandler` (without `KeyboardHandler`): a tutorial showing that "any
   bulk/interrupt device can be implemented with read/write alone". Key steps for HID devices:

   ```cpp
   // 1. Construct HID interface descriptor (Boot keyboard, interrupt IN endpoint)
   auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
   // 2. Set HID descriptor (0x21): appended after the interface descriptor, necessary for driver loading
   pipe->set_class_specific_descriptor({0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00});
   // 3. Set the report descriptor (0x22): returned on GET_DESCRIPTOR
   pipe->set_custom_descriptor(0x22, keyboard_report_descriptor);
   // 4. The data plane consists of blocking read/write
   pipe->write(PipeXfer{.ep = 0x81, .data = {0x00, 0x00, 0x04, 0, 0, 0, 0, 0}}); // Press the A key
   ```

   Control requests (GET_REPORT / SET_REPORT and other class requests) are returned by read() as a
   PipeXfer carrying setup_req; respond in the IN direction with `write(PipeXfer{.ep = 0, .data = ...})`.

**19. mock_ecm**

   Virtual Ethernet card (CDC ECM, appears as a network card on the host). Two backends:

   - Default: user-space `EthernetEchoBackend` - respond to ARP/ICMP/TCP echo (no root required,
     Available on all platforms)
   - `--tun <interface name>` (Linux/Android only, requires root): `TunBackend` connects the virtual
     network adapter to the local kernel protocol stack through a TAP interface (such as `usbip%d`); the real kernel handles ARP/ICMP/TCP and routing

   Usage: `mock_ecm --tun usbip%d`, after `ip addr add 192.168.53.2/24 dev usbX` on the host side
   `ping 192.168.53.1`. macOS has no TAP interface (utun is a layer-3 device, and the tuntaposx driver is disabled on newer
   systems), so the `--tun` option is not compiled on macOS.

**20. libusb_windows_service**

   Wraps the libusb server as a **Windows service** (Windows only). Run using Windows SCM API
   `LibusbServer`, supports complete service lifecycle management (via `net start`/`net stop` or
   `sc.exe` start and stop). Supports automatic binding of all connected USB devices on startup.

**21. mock_rndis**

   Virtual Ethernet adapter (CDC RNDIS): **no additional Windows driver required** ("Remote NDIS Compatible Device"). This is the key difference from
   `mock_ecm` (ECM requires a third-party driver on Windows). Backends and usage match
   mock_ecm (default user-space echo, or `--tun` on Linux/Android). The RNDIS control channel
   (CDC encapsulated commands for INIT/QUERY/SET), OID table, and data encapsulation all follow the Linux kernel gadget
   implementation (f_rndis.c/rndis.c).

   Usage: `mock_rndis --tun usbip%d`, after `ip addr add 192.168.53.2/24 dev usbX` on the host side
   `ping 192.168.53.1`.

**22. mock_webcam**

   Virtual **UVC Camera + UAC Microphone** Composite device - a single USB device presents the camera and microphone simultaneously,
   matching a real external webcam, **without an additional Windows driver** ("USB camera" and "microphone" enumerate simultaneously,
   just as with a physical webcam). The UVC function group declares an IAD; camera (ISO IN video frames) and microphone (ISO IN
   audio) stream simultaneously from the server.

   Usage: `mock_webcam --width 640 --height 480 --fps 30 --rates 48000` (mock_uvc’s
   `--width/--height/--fps` merged with `--freq/--rates/--channels/--amp/--harmonics/--audio` of mock_audio).

---

## Architecture design

USB communication and network communication are both I/O-intensive tasks. The architecture combination of this project is as follows:

- Using **asio** to handle network I/O - connection reception is asynchronous (coroutine driven accept loop on network thread)
- Use **synchronous blocking socket I/O** in each session thread - simple to write and easy to reason
- Handle USB communication (physical device) using libusb asynchronous interface

### Why Session I/O Is Synchronous

Earlier versions used C++20 coroutines for session data, but later switched to synchronous blocking socket I/O for these reasons:

1. **Architecture mismatch**: This project adopts the "one thread per connection" model, and each client connection has an independent thread and `io_context`. The core advantage of coroutines is "single thread processing multiple tasks", which cannot be brought into play under this architecture.

2. **Code complexity**: The logic of the coroutine version and the non-coroutine version is almost the same, but two sets of codes need to be maintained, which increases maintenance costs.

3. **Compilation overhead**: Coroutine-related template instantiation will significantly increase the compilation time.

4. **ESP32 Consideration**: For embedded platforms, it is more recommended to use FreeRTOS native tasks instead of coroutines, or to use a single-thread event loop architecture instead.

The network thread's accept loop (`Server::do_accept`) still uses a single coroutine: it is sufficiently simple and self-contained that none of the above issues apply.

If you need to support a large number of concurrent connections (hundreds/thousands) in the future, you can consider refactoring it into a single `io_context` + coroutine model, then the advantages of coroutines can be truly reflected.

### Dependencies

| Dependency | Required? | Description |
|------|----------|------|
| asio | ✅ Required | Asynchronous I/O library |
| spdlog | ✅ Required | Log library |
| libusb-1.0 | Optional | For physical USB device forwarding |
| libevdev | Optional (Linux only) | For evdev input device forwarding |
| cxxopts | Optional | Used to compile sample programs |
| GTest | Optional | For building tests |
| miniaudio + stb | Optional | mock_audio audio-file playback (--audio) and mock_speaker local playback (header-only) |

### Platform support

| Platform | Virtual Device | Physical Device (libusb) | Remarks |
|------|----------|-------------------|------|
| Windows | ✅ | ⚠️ WinUSB driver required | Suitable for virtual HID devices |
| Linux | ✅ | ✅ | Fully supported |
| macOS | ✅ | ✅ | Fully supported |
| Android (Termux) | ✅ | ✅ Via termux-usb | Supports non-root access |
| ESP32 | ✅ | ✅ | Using ESP-IDF and asio components |

### Core classes

| Class | Description |
|----|------|
| `Server` | Main server class, manages the device list and accepts connections |
| `Session` | Represents client connection and handles USBIP protocol |
| `UsbDevice` | USB device descriptor and configuration |
| `AbstDeviceHandler` | Abstract base class for all device handlers |
| `LibusbServer` | Server wrapper for forwarding physical USB devices (based on libusb) |
| `StringPool` | Manage USB string descriptors (up to 255) |

### Release semantics after the device is removed

After a device is physically removed (`AbstDeviceHandler::is_device_removed()` returns true; after receiving a system removal notification, the backend informs the handler through `on_device_removed()` or similar paths, and the handler decides when to set the flag):

- **Discard on release**: On session disconnection / `stop()`, discard the device directly from the "in-use" list instead of returning it to the available list. The backend scans the list only at the moment the device disappears; returning it would leave an unremovable zombie (and merely incur a failed open on the next import)
- **Import must fail**: Removed devices that still remain in the list cannot be opened when imported, and will also be discarded after failure.
- No background scan or fallback cleanup on session exit is therefore needed; zombies are prevented at their source

### Transfer data carriers

Transfer data is managed by [`TransferHandle`](include/protocol.h), a RAII wrapper that automatically frees the transfer handle on destruction. Move semantics transfer ownership; note that `release()` relinquishes ownership, requiring manual release.

### Tools

| Class | Description |
|----|------|
| `ObjectPool<T, PoolSize, ThreadSafe, LifeManager, Reset>` | Fixed size object pool, supporting custom creation/destroy/reset strategies. alloc O(1), free O(log n). |

### Virtual device class

| Class | Description |
|----|------|
| `VirtualDeviceHandler` | Base class for virtual USB devices |
| `SimpleVirtualDeviceHandler` | Simple device handler, providing empty implementation of standard requests |
| `VirtualInterfaceHandler` | Base class for implementing virtual USB interface |
| `HidVirtualInterfaceHandler` | HID device base class (mouse, keyboard, etc.) |
| `AbsoluteMouseHandler` | Absolute coordinate mouse, supports screen coordinate mapping and humanized movement |
| `RelativeMouseHandler` | Relative-coordinate mouse, accumulates offsets for transmission, 5 buttons + scroll wheel |
| `KeyboardHandler` | USB HID keyboard with built-in Consumer Control media key support |
| `GamepadHandler` | USB HID gamepad, 16 buttons + D-pad + 4 analog axes |
| `DigitizerHandler` | USB HID touchscreen with pressure support |
| `MscBulkOnlyHandler` | USB mass storage BOT protocol handler implementing SCSI commands |
| `StorageBackend` | Block storage backend abstract interface, providing read and write capabilities for MSC devices |
| `RawImageBackend` | Memory-mapped disk image file backend (cross-platform) |
| `MemoryBackend` | Memory-based block storage backend for MSC testing |
| `CdcAcmCommunicationInterfaceHandler` | CDC ACM communication interface handler |
| `CdcAcmDataInterfaceHandler` | CDC ACM data interface handler |
| `UvcVideoControlHandler` | UVC VideoControl interface (camera control, status interrupt) |
| `UvcVideoStreamingHandler` | UVC VideoStreaming interface (PROBE/COMMIT, ISO video streaming) |
| `VideoSource` | UVC virtual camera video source abstract interface |
| `ColorBarSource` | Color-bar test-pattern video source |
| `UacAudioControlHandler` | UAC AudioControl interface (Feature Unit mute/volume control) |
| `UacAudioStreamingSourceHandler` | UAC AudioStreaming interface (ISO PCM push streaming) |
| `AudioSource` | UAC virtual microphone PCM audio source abstract interface |
| `UacAudioStreamingSinkHandler` | UAC AudioStreaming interface (ISO OUT stream reception/consumption, speaker direction) |
| `AudioSink` | UAC virtual speaker PCM consumer abstract interface |
| `SineWaveSource` | Sine wave test sound source |
| `FourierSource` | Fourier series synthetic sound source (multiple harmonics superimposed, each harmonic has independent phase) |
| `AudioFileSource` | Audio source for mock_audio example (WAV/MP3/FLAC/OGG, based on miniaudio, supports resampling and looping; in examples/mock_audio/) |
| `EcmCommunicationInterfaceHandler` | CDC ECM communication interface (Ethernet connection/status notification) |
| `EcmDataInterfaceHandler` | CDC ECM data interface (Ethernet frame sending and receiving) |
| `NetworkBackend` | Network backend abstraction of virtual Ethernet card (frame sending and receiving + statistics) |
| `EthernetEchoBackend` | mock_ecm user-space echo backend (ARP/ICMP/TCP echo replies, no root required) |
| `TunBackend` | Linux TAP backend: feed virtual network adapter frames into the local kernel protocol stack (Linux/Android only, requires root) |
| `RndisCommunicationInterfaceHandler` | CDC RNDIS communication interface (encapsulated command channel, RNDIS state machine and OID table) |
| `RndisDataInterfaceHandler` | CDC RNDIS data interface (RNDIS_MSG_PACKET encapsulation/decapsulation, frame transmission and reception) |

### Class inheritance relationship

```
AbstDeviceHandler
├── LibusbDeviceHandler    (physical devices through libusb)
└── VirtualDeviceHandler   (virtual devices)
    └── SimpleVirtualDeviceHandler
```

### Thread model

Threads used by a server instance:

1. **Network I/O thread**: one for each `Server` - running `asio::io_context::run()` and accept coroutines, waiting for client connections
2. **Session thread**: one per connection - synchronously blocking socket I/O and device handler calls
3. **Sender thread**: One per transfer — take data from the response queue and write to the socket
4. **libusb event thread**: Run `libusb_handle_events()` loop, only used by libusb backend (`LibusbServer`)
5. **Main thread**: Call `start()` / `stop()`, control server

Each connection uses an independent thread to ensure that blocking operations on one connection (such as device synchronization) will not block other connections.

`start()` / `stop()` can be called repeatedly to restart the server. `stop()` closes the listener, interrupts all active sessions, and joins all session threads before returning; `Server` can then be safely destroyed.

data transfer process:

```
client → session thread (read URB) → device handler → sender thread (write response) → client
```

This architecture achieves high CPU efficiency by minimizing thread contention.

### Virtual device implementation

When developing virtual devices, please note:

- Avoid blocking network threads
- Process the request in the working thread
- Submit response data through callback

#### Why do we need a request queue?

The client is only responsible for sending URBs, and the server is responsible for receiving them. In the original USBIP, the server will store the URB after receiving it, and then send it to the real device in order by the USB controller. This ensures that only one URB is being transferred at the same time.

The virtual device needs to simulate this "store URBs in order" behavior, so the request queue (`std::deque`) is used to store the received requests and process them one by one in order.

## Platform description

### Windows: libusb needs to replace the driver

When using libusb server on Windows, you need to replace the driver:

1. Use [Zadig](https://zadig.akeo.ie/) to install the WinUSB driver
    - Select target device (enable "List all devices" when device not found)
    - **WARNING**: Replacing the mouse/keyboard driver will cause input failure
2. After use, roll back the driver through the device manager:
    - `Win+X` → Device Manager → Select Device → Roll Back Driver

For physical devices, it is recommended to use [usbipd-win](https://github.com/dorssel/usbipd-win),
That project implements the above functionality at the driver level using VBoxUSB
This project is more suitable for realizing **virtual USB device** in Windows

---

## Contribution Guide

### Code description

> 📝 **Language policy**: Public API documentation (doxygen) and runtime messages use **English**. Internal implementation comments may use **Chinese or English** — these are the project's only permitted languages; PRs in other languages are not accepted.

### Branch workflow

This project uses a single main branch workflow:

- Fixes and minor changes are committed directly to main
- Larger or experimental changes are developed in a separate branch and merged back into main after completion
- External contributor: fork this repository and submit a PR for main
- Releases: update the version in CMakeLists.txt, commit and push, then push a v* tag; CI automatically builds and publishes packages

---

## License

This project uses the [LGPLv3](LICENSE) license.

> **Attribution required**: Any use of this library — including source-code inclusion, dynamic/static linking, modified
> derivative works, and distribution in any product or service — **must** clearly identify its use in a prominent location (such as the product's "About/
> Acknowledgments" page or documentation):
>
> ```text
> This product uses usbipdcpp (https://github.com/yunsmall/usbipdcpp) and follows the LGPLv3 license.
> ```

usbipdcpp is a project I maintain alone in my spare time as a labor of love. For a library like this, attribution is often the main way users
learn that it exists — that is the purpose of the requirement above.

For closed-source distribution after modifying this library's code, please contact: yun_small@163.com

---

## Who is using usbipdcpp

- [moonlight-qt](https://github.com/qiin2333/moonlight-qt) —— USB forwarding helper on macOS
  (`moonlight-usbd`, packaged with macOS version)
- [moonlight-vplus](https://github.com/qiin2333/moonlight-vplus) —— Android USB/IP
  export backend (`usbip-backend`)

Have you built something with usbipdcpp too? Submit a PR to add a line here — remember to provide attribution as described in the [License](#license) section.

---

## Acknowledgments

This project benefits from the following open source projects:

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)
