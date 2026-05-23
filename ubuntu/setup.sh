#!/usr/bin/env bash
# Net-Rewire VPS setup — one-shot deployment script
# Run this on the Ubuntu VPS to install the tunnel server and inbound forwarding.
#
# Usage:
#   MAILCOW_TS_IP=100.x.y.z bash setup.sh
#
# What it does:
#   1. Install build dependencies
#   2. Compile tunnel_server
#   3. Install systemd services
#   4. Apply and persist iptables inbound rules
set -euo pipefail

MAILCOW_TS_IP="${MAILCOW_TS_IP:?set MAILCOW_TS_IP to your Mailcow Tailscale IP}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Net-Rewire VPS Setup ==="
echo "Mailcow Tailscale IP: ${MAILCOW_TS_IP}"

# --- 1. Install dependencies ---
echo ""
echo "--- Installing dependencies ---"
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential iptables-persistent

# --- 2. Compile tunnel server ---
echo ""
echo "--- Building tunnel server ---"
cd "$SCRIPT_DIR"
gcc -Wall -Wextra -O2 -std=c99 -o net-rewire-tunnel-server tunnel_server.c -lpthread
sudo cp net-rewire-tunnel-server /usr/local/bin/net-rewire-tunnel-server
sudo chmod +x /usr/local/bin/net-rewire-tunnel-server

# --- 3. Install systemd services ---
echo ""
echo "--- Installing systemd services ---"

# Tunnel server
sudo cp "$SCRIPT_DIR/tunnel-server.service" /etc/systemd/system/net-rewire-tunnel-server.service
sudo systemctl daemon-reload
sudo systemctl enable net-rewire-tunnel-server
sudo systemctl restart net-rewire-tunnel-server

# --- 4. Apply iptables inbound rules ---
echo ""
echo "--- Applying inbound iptables rules ---"
sudo bash "$SCRIPT_DIR/inbound-forward.sh" apply

# --- 5. Enable IP forwarding ---
echo ""
echo "--- Enabling IP forwarding ---"
sudo sysctl -w net.ipv4.ip_forward=1
if ! grep -q 'net.ipv4.ip_forward.*=.*1' /etc/sysctl.conf; then
    echo 'net.ipv4.ip_forward=1' | sudo tee -a /etc/sysctl.conf > /dev/null
fi

# Save iptables rules persistently
echo ""
echo "--- Saving iptables rules ---"
sudo netfilter-persistent save

# --- Done ---
echo ""
echo "=== Setup complete ==="
echo ""
echo "Services:"
sudo systemctl status net-rewire-tunnel-server --no-pager -l || true
echo ""
echo "iptables rules:"
sudo bash "$SCRIPT_DIR/inbound-forward.sh" show
echo ""
echo "Verify from outside: telnet <vps-public-ip> 25"
