#!/usr/bin/env bash
# ecm end-to-end: network adapter appears after attach (enx+MAC); configure IP and ping device side
# (mock_ecm default echo backend answers ARP/ICMP entirely in userspace; server needs no root)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir ecm
echo "== ecm: Start server + attach"
start_server mock_ecm || { report ecm; exit 1; }
attach_device 1-1

NETDEV=$(wait_netdev 10) || true
assert "Network adapter appears ($NETDEV)" [ -n "$NETDEV" ]
[ -n "$NETDEV" ] || { stop_server; report ecm; exit 1; }

# Configure IP and enable (ping target 192.168.53.1 is device-side address; echo backend responds)
sudo ip addr add 192.168.53.2/24 dev "$NETDEV"
sudo ip link set "$NETDEV" up
sleep 1
assert "ping 192.168.53.1 succeeds" ping -c 3 -W 1 192.168.53.1

# TCP echo: connect to device-side port 5000, send 5 bytes, verify unchanged echo (echo backend echoes by segment)
TCP_REPLY=$(timeout 5 bash -c 'exec 3<>/dev/tcp/192.168.53.1/5000; printf "hello" >&3; IFS= read -r -N 5 -t 3 reply <&3; printf "%s" "$reply"' 2>/dev/null || true)
assert "TCP echo 5000 echoes ($TCP_REPLY)" [ "$TCP_REPLY" = "hello" ]

# Clean up: restore adapter IP (avoid affecting enx state in next test)
sudo ip addr flush dev "$NETDEV" 2>/dev/null || true
stop_server
report ecm
