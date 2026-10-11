#!/usr/bin/env python3
# mock_pipe data-plane test: send message via libusb bulk OUT → read echo via bulk IN,
# verifying entire vendor-class device transfer chain (mock echoes received OUT unchanged to IN).
# IN also contains mock periodic count messages (one every 2 seconds); read repeatedly until this message's content appears.
import sys
import time

import usb.core
import usb.util

dev = usb.core.find(idVendor=0x1234, idProduct=0x5690)
if dev is None:
    print("ERROR: Cannot find 1234:5690 (attached?)")
    sys.exit(2)

msg = b"PY_PIPE_TEST_" + str(int(time.time())).encode()
dev.set_configuration()
usb.util.claim_interface(dev, 0)

written = dev.write(0x02, msg, timeout=2000)  # bulk OUT ep 0x02
got = b""
deadline = time.time() + 6
while time.time() < deadline and msg not in got:
    try:
        chunk = dev.read(0x81, 4096, timeout=1000)  # bulk IN ep 0x81
        got += bytes(chunk)
    except usb.core.USBError:
        continue  # Keep waiting after timeout (periodic messages also appear on IN)
print("sent:", written, "bytes")
print("got:", got)
print("match:", msg in got)
sys.exit(0 if msg in got else 1)
