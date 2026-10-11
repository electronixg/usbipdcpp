#!/usr/bin/env bash
# pipe end-to-end: successful attach + server logs show periodic transfer activity
# (mock_pipe writes count message to IN channel every 2 seconds; vendor-class device has no standard host driver,
# no /dev node; verify attach state and server-side transfer logs)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir pipe
echo "== pipe: Start server + attach"
start_server mock_pipe || { report pipe; exit 1; }
attach_device 1-1

# Wait for successful attach (usbip port shows import)
sleep 1
PORTS=$(sudo usbip port 2>/dev/null || true)
assert "Device imported" grep -q "1-1 -> usbip://$HOST_IP:$PORT/1-1" <<<"$PORTS"

# Actual data plane: python(pyusb) sends one message via bulk OUT → reads echo via bulk IN.
# Vendor class has no host driver or /dev node; direct libusb access is the only data-plane verification
timeout 15 sudo python3 "$E2E_DIR/pipe_test.py" > "$WORK_DIR/pipe_test.out" 2>&1
assert "Data-plane transfer works (OUT echoed and read back through IN)" [ $? -eq 0 ]

# Server side: periodic-send thread log (count message every 2 seconds)
assert "Server log contains periodic sends" grep -q "Periodic send" "$LOG_DIR/mock_pipe.log"

stop_server
report pipe
