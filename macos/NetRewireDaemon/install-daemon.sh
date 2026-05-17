#!/usr/bin/env bash
# install-daemon.sh — Build and install net-rewire-daemon on macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DAEMON_BIN="/usr/local/bin/net-rewire-daemon"
PLIST_PATH="/Library/LaunchDaemons/com.netrewire.daemon.plist"
PF_ANCHOR="/etc/pf.anchors/net-rewire"
PF_CONF="/etc/pf.conf"

UBUNTU_HOST="${1:-}"
UBUNTU_PORT="${2:-12345}"

if [ -z "$UBUNTU_HOST" ]; then
    echo "Usage: $0 <ubuntu-server-ip> [port]"
    echo "Example: $0 203.0.113.5 12345"
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)."
    exit 1
fi

echo "=== Net-Rewire Daemon Installer ==="
echo "Ubuntu server: $UBUNTU_HOST:$UBUNTU_PORT"
echo ""

# ── 1. Build the daemon ─────────────────────────────────────────────

echo "--- Building daemon ---"
cd "$PROJECT_DIR"

DAEMON_SRC="$SCRIPT_DIR/main.c"
PKTPARSE_SRC="$PROJECT_DIR/NetRewirePacketTunnel/pktparse.c"
PKTPARSE_HDR="$PROJECT_DIR/NetRewirePacketTunnel"

if [ ! -f "$DAEMON_SRC" ]; then
    echo "ERROR: $DAEMON_SRC not found"
    exit 1
fi

cc -O2 -Wall -Wextra \
    -I"$PKTPARSE_HDR" \
    -o "$DAEMON_BIN" \
    "$DAEMON_SRC" \
    "$PKTPARSE_SRC" \
    -framework SystemConfiguration

echo "  -> $DAEMON_BIN"

# ── 2. Install launchd plist ────────────────────────────────────────

echo "--- Installing launchd plist ---"
cat > "$PLIST_PATH" << PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.netrewire.daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>$DAEMON_BIN</string>
        <string>$UBUNTU_HOST</string>
        <string>$UBUNTU_PORT</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/net-rewire.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/net-rewire.log</string>
    <key>ProcessType</key>
    <string>Interactive</string>
</dict>
</plist>
PLIST_EOF

echo "  -> $PLIST_PATH"

# ── 3. Enable PF and configure rules ────────────────────────────────

echo "--- Configuring Packet Filter ---"

# Enable IP forwarding (needed for PF route-to on macOS)
sysctl -w net.inet.ip.forwarding=1 2>/dev/null || true

# Write PF anchor rules
mkdir -p "$(dirname "$PF_ANCHOR")"
cat > "$PF_ANCHOR" << 'PF_EOF'
# Net-Rewire SMTP tunnel
# This anchor is loaded from /etc/pf.conf
pass out proto tcp from any to any port 25
PF_EOF
echo "  -> $PF_ANCHOR"

# Ensure pf.conf loads our anchor
if ! grep -q "net-rewire" "$PF_CONF" 2>/dev/null; then
    cat >> "$PF_CONF" << 'PF_CONF_EOF'

# Net-Rewire SMTP tunnel anchor
anchor "net-rewire"
load anchor "net-rewire" from "/etc/pf.anchors/net-rewire"
PF_CONF_EOF
    echo "  -> Added anchor to $PF_CONF"
fi

# Enable PF
pfctl -e 2>/dev/null || true

# Load/reload rules
pfctl -f "$PF_CONF" 2>&1 || {
    echo "WARNING: pfctl load had errors (may be harmless if anchor already loaded)"
}

echo "  -> PF enabled with net-rewire rules"

# ── 4. Load the daemon ──────────────────────────────────────────────

echo "--- Starting daemon ---"
launchctl unload "$PLIST_PATH" 2>/dev/null || true
launchctl load "$PLIST_PATH"

sleep 1

if launchctl list com.netrewire.daemon &>/dev/null; then
    echo "  -> Daemon is running"
else
    echo "  WARNING: Daemon may not have started. Check:"
    echo "    sudo launchctl list com.netrewire.daemon"
    echo "    tail -f /var/log/net-rewire.log"
fi

# ── 5. Verify ───────────────────────────────────────────────────────

echo ""
echo "=== Installation complete ==="
echo ""
echo "Verify with:"
echo "  sudo launchctl list com.netrewire.daemon"
echo "  ifconfig utun0 utun1 utun2 2>/dev/null | grep -A5 'inet 10.8'"
echo "  sudo pfctl -s Anchors | grep net-rewire"
echo "  nc -v smtp.example.com 25"
echo ""
echo "Logs: tail -f /var/log/net-rewire.log"
