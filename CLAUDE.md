Be very careful when modifying protocol parsing: this part is the result of extensive debugging. Other architectural components may be modified.

When parsing/constructing protocol fields (USB descriptors, Ethernet/IP/TCP headers, etc.), use packed structures and access named member fields.
Do not use std::vector or std::array with manually calculated byte offsets for reads/writes (offsets are easy to miscalculate, and adding/removing fields cannot automatically expose errors).
Store multibyte fields in memory in network byte order; explicitly convert them on access using functions such as bswap/htons.

## Viewing the Test Summary

When running ctest, a single command must show the pass/fail counts. Do not run it twice: retain at least 15 lines with tail
(the summary line `100% tests passed out of N` is in the last two output lines; a tail that is too short truncates the summary
and requires another run):

```bash
ctest --test-dir build --output-on-failure -j12 2>&1 | tail -15
```

## Comment Style

Documentation comments for functions/methods/structures must use Doxygen style (`/** ... */`, @brief summary +
@param parameter descriptions + @return return-value description), matching the format in InterfaceHandler.h for
AbstInterfaceHandler::on_new_connection:

```cpp
    /**
     * @brief Called when a new client connects
     * @param responder Transfer response interface (for submitting responses/stopping transfers)
     * @param ec The resulting error code
     */
```

Do not use `///` or `//` to document functions/methods/structures (inline comments explaining "why" are unrestricted).

## Debugging When spdlog Log Levels Are Removed at Compile Time

In Release/RelWithDebInfo builds, SPDLOG_ACTIVE_LEVEL is set to INFO at compile time, so TRACE/DEBUG logs
are not emitted (setting spdlog::set_level at runtime has no effect). To view TRACE/DEBUG logs for a particular file,
**do not raise the log level line by line**. Follow the approach at the top of Session.cpp: at the very top of that cpp file,
before the includes, forcibly redefine the macro to enable all log levels for that file; remove it after debugging:

```cpp
// For debugging: <state why this is enabled; remove after troubleshooting>
#ifdef SPDLOG_ACTIVE_LEVEL
    #undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
```

When changing APIs, consider how difficult they are to implement on embedded platforms of this kind

This project's open-source license is lgpl; do not confuse it with another license

Do not create git commits proactively; commit only when I ask you to

Do not append "_" to member variable names

Commit the code before pushing; execute the push and commit separately, not together

When creating a new version tag, remember to update the version number in CMakeLists.txt

All compiled binaries (mock_* from examples, test_* from tests, etc.) are placed in the build directory root (under build*), never in subdirectories; do not confuse their locations

## git Workflow

Single main branch model (develop has been deleted):

- Routine commits go directly to main
- Large experimental features: develop on a temporary feature branch, then merge back into main and delete the branch
- External contributors: fork + PR to main
- Releases: update the version number in CMakeLists.txt, commit and push, then create and push a v* tag; CI automatically builds release packages

## Using usbip Commands

Assuming a usbip server is listening on local port 53240, use commands such as
`usbip -t 53240 attach/list ...` for attach or list. Note that -t must immediately follow usbip,
before all subcommands such as attach and list

**usbip attach must use sudo**: attach writes to /sys and hands the fd over to vhci_hcd.
If the default WSL user is not root, insufficient permissions produce `usbip: error: import device` (easily mistaken for
a server/protocol problem). list is read-only and does not require sudo, but for consistency you can use
`sudo usbip -t 53240 ...` directly. Verify the import status with `sudo usbip port`.

**Windows usbip.exe and the Linux usbip tool use exactly the same commands**: -t/--tcp-port is
a shared top-level global option (immediately after usbip, before all subcommands). Windows and Linux
have corresponding attach/list/port syntax. To connect from Windows to a server on a port other than 3240, use
`usbip -t 53240 list -r <ip>` / `usbip -t 53240 attach -r <ip> -b 1-1`,
Do not assume port selection is unsupported because the attach subcommand's help lists -t as --terse
(--terse is an option of the attach subcommand itself; top-level -t selects the port, as shown in the top-level OPTIONS
of `usbip --help`). Importing a device exported from Windows (usbipd-win) on Linux uses
exactly the same syntax: `usbip -t <port> attach -r <Windows host ip> -b <busid>`.

## Pitfalls in Local Validation of mock Devices

When starting a mock server in WSL and validating it with attach, note the following:

- **Starting**: mock servers (wait_for_exit) wait for exit signals (POSIX: SIGINT/SIGTERM;
  Windows: console events); background execution does not require stdin to remain attached. Use
  `setsid ./mock_xxx -p 53240 < /dev/null > log 2>&1 &`
- **Stopping**: use `kill -TERM <pid>` or `pkill -x mock_xxx` (both send SIGTERM).
  The server follows its normal cleanup path (server.stop: closes connections and releases the port) and exits gracefully.
  After exit, use `ss -tln | grep 53240` to confirm the port was released (an occupied port prevents the next
  server from starting and can cause an accidental attach to the old device)
- **Do not use `pkill -f mock_xxx` to clean up old processes**: `-f` matches the entire command line
  and can kill its own shell (whose bash -lc command string contains "mock_xxx"). Instead, match the exact
  process name with `pkill -x mock_xxx` (the executable name, without paths/arguments)
- **The kernel truncates process names longer than 15 characters** (the Linux comm limit): `pkill -x` matches
  the truncated name, for example `mock_cdc_throttle`→`mock_cdc_thrott`,
  `multi_interface_hid`→`multi_interface`; `multi_devices` is exactly 13 characters and is not truncated.
  If termination fails, first inspect the actual comm name with `ps -eo comm | grep -i mock`, then use `pkill -x <actual name>`

**MSC (USB flash drive) block-device reads/writes must be performed in separate steps; do not combine discovery and writing in one command**:
After attach, first identify the newly appeared block device separately (for example, compare `lsblk` before/after attach,
or use `dmesg | tail` to see the sdX name enumerated by the kernel). After confirming it, **run the read/write command separately**.
Never chain "discover device + write device" into a single command such as
`lsblk ... && dd if=/dev/sdX ...`: WSL contains other physical disks, and writing to the wrong disk destroys data

**Use `/dev/input/by-path/` to locate devices for evtest**: under WSL's devtmpfs, `/dev/input/eventX`
node numbers do not necessarily match `/sys/class/input/inputX` numbers (making the wrong node easy to select). The by-path
symlinks identify the actual device (for example `platform-vhci_hcd.0-usb-0:1:1.0-event-kbd -> ../event0`).
First find the target device name with `ls /dev/input/by-path/`, then run `sudo evtest /dev/input/by-path/<name>`.
