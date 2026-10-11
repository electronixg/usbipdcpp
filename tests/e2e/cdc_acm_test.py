#!/usr/bin/env python3
# cdc_acm echo test helper: one fd for termios setup + write + readback.
# Usage: cdc_acm_test.py <serial-device> <string-to-write>; on success, output readback content (no newline).
# Note: must open O_RDWR (open submits a pending read); writing first (tee), then opening reader (dd) in sh
# uses two processes with uncontrollable timing; tests showed no echo when reader opened late.
import os
import sys
import termios
import time

dev = sys.argv[1]
msg = sys.argv[2].encode()

fd = None
# Driver may not be ready immediately after enumeration; retry open + termios setup
for _ in range(10):
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(fd)
        a[0] &= ~(termios.ICRNL | termios.IXON)            # Input conversion/software flow control
        a[1] &= ~termios.ONLCR                             # Output NL->CRNL
        a[2] &= ~termios.CRTSCTS                           # Hardware flow control
        a[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)  # echo/canonical mode
        a[6][termios.VMIN] = 1
        a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        break
    except OSError:
        if fd is not None:
            os.close(fd)
            fd = None
        time.sleep(0.5)
if fd is None:
    sys.exit(2)

os.write(fd, msg)
time.sleep(1)  # Wait for mock echo to reach tty read buffer
os.set_blocking(fd, False)
buf = b""
deadline = time.time() + 3
while time.time() < deadline and len(buf) < len(msg):
    try:
        chunk = os.read(fd, len(msg) - len(buf))
        if not chunk:
            break
        buf += chunk
    except BlockingIOError:
        time.sleep(0.05)
os.close(fd)
sys.stdout.write(buf.decode(errors="replace"))
