#!/usr/bin/env bash
# Net-Rewire iptables persistence
# Saves current iptables rules and ensures they survive reboots.
# Uses netfilter-persistent (iptables-persistent package).
set -euo pipefail

echo "=== Net-Rewire iptables persistence ==="

# Install if not present
if ! command -v netfilter-persistent &> /dev/null; then
    echo "Installing iptables-persistent..."
    sudo apt-get update -qq
    sudo apt-get install -y -qq iptables-persistent
fi

# Save current rules (both IPv4 and IPv6)
echo "Saving current iptables rules..."
sudo netfilter-persistent save

echo ""
echo "Rules saved. They will be restored on boot."
echo ""
echo "Current rules:"
echo "--- PREROUTING ---"
sudo iptables -t nat -L PREROUTING -n -v
echo "--- FORWARD ---"
sudo iptables -L FORWARD -n -v
echo "--- POSTROUTING ---"
sudo iptables -t nat -L POSTROUTING -n -v
