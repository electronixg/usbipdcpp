# e2e end-to-end tests

Test scripts in this directory run under WSL2 (Ubuntu): start mock server → local usbip client
attach → verify virtual-device behavior (network/serial/block device/HID/audio/video).

> **Recommendation**: Run this test suite in a virtual machine where possible (WSL2 /dev directly contains
> Windows physical disks, so damage is still possible), avoiding effects of test device enumeration, network changes,
> and block-device operations on the physical machine's everyday environment.

## Dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| WSL2 (Ubuntu) | Test environment | Scripts execute inside WSL; invoke from Windows using `wsl -e bash` |
| Passwordless `sudo` | Required for usbip attach/detach | vhci writes to /sys require root; otherwise scripts stall awaiting password |
| usbip client (`/usr/local/sbin/usbip`) | attach/list/detach | Kernel usbip userspace tool, supports `-t <port>`; **sudo required** |
| usbip-win2 vhci kernel module | USB bus emulation | Confirm loaded with `lsmod \| grep vhci`; otherwise attach fails |
| Build output (`build_wsl2/`) | Mock server binaries | First `cmake --build build_wsl2 -j6` (build in WSL, dependencies through vcpkg) |
| python3 + pyusb (`python3-usb` package) | cdc_acm / pipe data-plane tests | Both scripts fail without them |
| alsa-utils (aplay/arecord) | audio / speaker tests | Corresponding script SKIPs or fails if missing |
| ffmpeg | uvc stream retrieval test | SKIP if missing |
| evtest | HID (keyboard/mouse/gamepad) tests | Event-stream capture |
| iproute2 / util-linux / coreutils | ss, lsblk, blockdev, truncate, cmp, etc. | Included by default in WSL |

## Preparation before testing

1. **Build**: `cd project-root && cmake --build build_wsl2 -j6` (execute in WSL; build directory in
   CLAUDE.local.md; do not use all cores, at most -j6)
2. **Confirm vhci module loaded**: `lsmod | grep vhci`—attach fails immediately if not loaded
3. **Confirm sudo available and passwordless**: `sudo -n true` should return 0
4. **Clean environment** (scripts do this on startup; check before manual runs):
   - No leftover listener on port 53240 (`ss -tln | grep 53240`)
   - No leftover devices in `/dev/input/by-path/`, `/dev/ttyACM*`, `/proc/asound/cards`
5. **Disk safety**: confirm physical disk device names attached in WSL (`lsblk`); scripts identify mock_msc
   new disk through two independent checks, but manual interference (below) may break confirmation logic

## Running

```bash
# Single test
bash tests/e2e/test_msc.sh

# All tests (run sequentially, continue on failure)
bash tests/e2e/run_all.sh
```

- Separate working directory per script (`/tmp/usbip_e2e/<test-name>`); logs in
  `<working-directory>/logs/`; server runs in background with `setsid`; automatic cleanup when script ends
- Scripts assume local port 53240 accessible (overridable using `PORT` environment variable)
- Mock server binaries found by default in `build_wsl2/` (overridable with `BUILD_DIR` environment variable,
  e.g. `BUILD_DIR=/path/to/build_wsl2_asan bash tests/e2e/test_msc.sh`)

## Prohibited actions during tests

1. **Do not combine MSC device lookup and writing**: USB flash-drive test script strictly separates steps (confirm new disk → verify size →
   write → read back); **manually** writing `lsblk && dd if=/dev/sdX ...` after attach
   combines lookup and writing in one command—other physical disks exist in WSL; wrong device name destroys data
2. **Do not use `pkill -f mock_xxx`**: `-f` matches entire command line and kills the
   shell running the test itself (command line contains mock_xxx). Use `pkill -x mock_xxx` for cleanup
   (executable name; kernel truncates beyond 15 characters; confirm actual name first with `ps -eo comm`)
3. **Do not manually attach/detach other usbip devices during tests**: this contaminates new-device discovery
   (block-device comparison, dmesg additions, input-device enumeration), causing misidentification or timeout
4. **Do not occupy port 53240**: if another program holds it during testing, server cannot start,
   and attach connects to old server (unexpected behavior: attach succeeds but device is old)
5. **Do not plug/unplug physical USB devices during testing**: newly inserted physical disks/input devices may be
   misidentified as virtual devices by lsblk comparison / by-path enumeration; at best test fails, at worst (physical disk
   mistaken for mock_msc disk) writes target wrong device
6. **Do not manually kill mock server under test**: scripts manage server lifecycle;
   manual kill disrupts cleanup logic (detach, waiting for port release)

## Risks

- **Data corruption (highest risk)**: MSC test writes to block device. Script safeguards:
  ① compare lsblk snapshots before/after attach ② cross-check kernel dmesg enumerated device name (both must agree)
  ③ `blockdev --getsize64` size must equal mock image (2MiB=2097152); any mismatch
  **aborts immediately without any writes**. Manually violating "prohibited actions" bypasses these safeguards
- **Leftover server/old session**: mock process remaining after previous crash occupies port; new attach connects to
  old device. Scripts handle this (sudo kill port process + await port release); take care during manual debugging
- **Leftover false tty node**: after device detach, commands such as `tee` may create `/dev/ttyACM0`
  as regular file (not character device), causing unexpected stty/read/write failures. wait_dev accepts only
  character devices (`-c`) to prevent this; if manual stty reports
  "Inappropriate ioctl for device", first use `ls -la /dev/ttyACM*` to check whether it is a regular file
- **Serial canonical-mode trap**: serial ports default to icanon (canonical mode), returning reads by line; without newline
  reader never receives data. Scripts disable icanon/echo with python before reading/writing; before manual
  serial testing, use `stty -F /dev/ttyACM0 -icanon -echo -icrnl -onlcr`
- **Sound-card resources**: WSL2 has no physical sound card; audio/speaker depend on USB sound cards enumerated by vhci;
  card numbers change across consecutive tests; scripts retrieve current number from `/proc/asound/cards`
- **ECM test modifies network**: test_ecm adds 192.168.53.2/24 to enx adapter and pings;
  script cleans adapter afterward; devices on same subnet in LAN may conflict during testing

## Test descriptions

| Script | Device | Verification |
|---|---|---|
| test_cdc_acm.sh | Virtual serial port | python(termios) write/read with single fd; exact echo match |
| test_ecm.sh | Virtual network adapter | enx adapter + ping 192.168.53.1 |
| test_msc.sh | USB flash drive | lsblk+dmesg dual confirmation of new disk → size verification → sector 100 write/read cmp |
| test_keyboard.sh | HID keyboard | evtest captures KEY_A events |
| test_mouse.sh | HID mouse | evtest captures REL movement events |
| test_gamepad.sh | HID gamepad | evtest captures gamepad events |
| test_cdc_throttle.sh | Serial port (throttled) | ttyACM count reports |
| test_pipe.sh | vendor pipe | pyusb bulk OUT→IN echo + periodic-send logs |
| test_audio.sh | UAC microphone | arecord records actual audio for 2 seconds |
| test_speaker.sh | UAC speaker | aplay plays 440Hz sine wave; content-level comparison of received WAV frequency/peak-and-trough count |
| test_uvc.sh | UVC camera | ffmpeg retrieves 5 actual frames |

Python helpers: `cdc_acm_test.py` (serial read/write), `pipe_test.py` (libusb transfers),
`speaker_verify.py` (audio-content comparison).
