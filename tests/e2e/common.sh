#!/usr/bin/env bash
# Shared e2e functions: mock server start/stop / usbip attach-detach / device waiting / assertions
# Usage: source common.sh, then call functions; scripts need sudo usbip (vhci writes to /sys require root)
set -u

# Build directory (overridable with BUILD_DIR environment variable; default WSL2 build)
E2E_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$E2E_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build_wsl2}"

PORT="${PORT:-53240}"
HOST_IP="${HOST_IP:-127.0.0.1}"
WORK_DIR="${WORK_DIR:-/tmp/usbip_e2e}"
LOG_DIR="${LOG_DIR:-$WORK_DIR/logs}"

PASS_COUNT=0
FAIL_COUNT=0

# Working and log directories (separate working directory per test prevents interference)
setup_workdir() { # setup_workdir <test-name>
    WORK_DIR="/tmp/usbip_e2e/$1"
    LOG_DIR="$WORK_DIR/logs"
    rm -rf "$WORK_DIR"
    mkdir -p "$LOG_DIR"
}

# Assertion utility: record pass for true condition, otherwise fail (script finally exits nonzero according to failure count)
assert() { # assert <description> <command...>
    local desc="$1"
    shift
    if "$@"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "  [PASS] $desc"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "  [FAIL] $desc"
    fi
}

# Server process management ==============================================================
# Note: pkill -x matches executable name; kernel truncates process names exceeding 15 characters (e.g.
# mock_cdc_throttle→mock_cdc_thrott); locating PID by port is most reliable (independent of process name)

# Kill process occupying PORT (wait for graceful exit).
# sudo required: non-root ss -p pid column is "-"; cannot obtain pid or kill server;
# leftover server occupies port, preventing new startup and making attach connect to old session
kill_by_port() {
    local pid
    pid=$(sudo ss -tlnp "sport = :$PORT" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)
    if [ -n "$pid" ]; then
        sudo kill "$pid" 2>/dev/null
    fi
}

# Wait for port ready (server starts listening)
wait_port_ready() { # wait_port_ready <timeout-seconds>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if ss -tln | grep -q ":$PORT "; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Wait for port release (reusable only after graceful server exit; accidental attach to old device)
wait_port_free() { # wait_port_free <timeout-seconds>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if ! ss -tln | grep -q ":$PORT "; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Start mock server: clear old processes → setsid background (< /dev/null prevents stdin hanging) → await port
start_server() { # start_server <binary-name> [arguments...]
    local bin="$1"
    shift
    detach_all # Clear leftover attachments (leftover sessions make new attach fail/connect to old device)
    kill_by_port
    wait_port_free || {
        echo "FAIL: Port $PORT not released (old server not killed?)"
        return 1
    }
    if [ ! -x "$BUILD_DIR/$bin" ]; then
        echo "FAIL: Cannot find $BUILD_DIR/$bin (build first?)"
        return 1
    fi
    cd "$WORK_DIR" 2>/dev/null || true
    setsid "$BUILD_DIR/$bin" "$@" < /dev/null > "$LOG_DIR/$bin.log" 2>&1 &
    if ! wait_port_ready; then
        echo "FAIL: $bin not listening on port $PORT (see $LOG_DIR/$bin.log)"
        return 1
    fi
    return 0
}

# Stop server: detach all devices first (otherwise sessions retain port), kill process + await port release
stop_server() {
    detach_all
    kill_by_port
    wait_port_free || echo "WARN: Port $PORT not released"
}

# usbip client ================================================================

# Attach device (sudo: vhci writes to /sys require root)
attach_device() { # attach_device <busid>
    sudo usbip -t "$PORT" attach -r "$HOST_IP" -b "$1"
}

# Detach all imported devices (parse port list line by line)
detach_all() {
    sudo usbip port 2>/dev/null | grep -oP '^Port \K[0-9]+' | while read -r p; do
        sudo usbip detach -p "$p" 2>/dev/null
    done
    sleep 0.5
}

# Device waiting ====================================================================

# Wait for network interface (ECM adapter named enx+MAC)
wait_netdev() { # wait_netdev <timeout-seconds>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        local dev
        dev=$(ip -br link | grep -oP 'enx[0-9a-f]+' | head -1)
        if [ -n "$dev" ]; then
            echo "$dev"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Wait for character device (serial /dev/ttyACM*, etc.).
# Accept only character devices (-c): if regular file remains after detach (accidentally created by tee),
# name matching would find false node (stty/read/write fail unexpectedly)
wait_dev() { # wait_dev <path-prefix> <timeout-seconds>
    local prefix="$1"
    local timeout="${2:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        local dev
        dev=$(ls "$prefix"* 2>/dev/null | while read -r d; do
            [ -c "$d" ] && echo "$d"
        done | head -1)
        if [ -n "$dev" ]; then
            echo "$dev"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Wait for usbip sound card (UAC) in /proc/asound/cards.
# grep -i: sound card named "Usbipdcpp ..." (uppercase U); case-sensitive matching misses it
wait_usb_soundcard() { # wait_usb_soundcard <timeout-seconds>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if grep -qi "usbip" /proc/asound/cards 2>/dev/null; then
            cat /proc/asound/cards
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Block devices (MSC)===============================================================
# Safety rules (see project CLAUDE.md): block-device reads/writes must be separate steps; after attach, first identify new device
# in a separate step, then separately perform reads/writes after confirming. Never combine device lookup and writing in one command—WSL
# also contains other physical disks; writing wrong device name destroys data.

# Record current block-device set (for before/after attach comparison)
snapshot_block_devices() {
    lsblk -n -o NAME | sort
}

# Identify newly added block device by comparison (separate step: query only, no changes)
# Return newly added sd device name (e.g. sdb); empty if none
find_new_block_device() { # find_new_block_device <pre-attach-snapshot>
    local before="$1"
    local after
    after=$(lsblk -n -o NAME | sort)
    comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | grep -E '^sd' | head -1
}

# Extract kernel-enumerated sd device name from post-attach dmesg additions (cross-check with lsblk comparison,
# double safeguard: disk enumerated by kernel must match disk found by lsblk comparison before acceptance)
dmesg_find_new_device() { # dmesg_find_new_device <pre-attach-dmesg-line-count>
    local before_lines="$1"
    sudo dmesg | tail -n +$((before_lines + 1)) | grep -oP '\[sd[a-z]+\]' | tail -1 | tr -d '[]'
}

# Verify block-device size equals expected (mandatory before writes: size mismatch means suspicious device identity,
# abort immediately, never write). mock_msc default 2MiB (4096 blocks × 512 bytes)
check_block_device_size() { # check_block_device_size <device-name> <expected-bytes>
    local dev="$1"
    local expected="$2"
    [ -b "/dev/$dev" ] || return 1
    local size
    size=$(sudo blockdev --getsize64 "/dev/$dev")
    [ "$size" = "$expected" ]
}

# Results ====================================================================
report() { # report <test-name>
    echo "=============================================="
    echo "$1: $PASS_COUNT passed, $FAIL_COUNT failed"
    [ "$FAIL_COUNT" -eq 0 ]
}
