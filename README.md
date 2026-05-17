# Net-Rewire

A macOS Network Extension (`NEPacketTunnelProvider`) that captures outbound TCP port 25 traffic and forwards it through a VPN tunnel to an Ubuntu server for transparent SMTP relay.

## Current Status

- The project has been converted from app-extension to **system-extension** (product type, Info.plist, MACH_O_TYPE, etc. all updated)
- Source compiles and links successfully, producing a 48KB Mach-O bundle
- Development team: **3Y9P65S34Z** (Andrew Saintway Personal Team)
- Entitlements embedded into binary via `ld -sectcreate __TEXT __entitlements`

### Core Bottleneck

Apple **free developer accounts do not support the Network Extensions capability**. Xcode explicitly errors:

> Personal development teams do not support the Network Extensions capability.

Without a valid provisioning profile, macOS will not load the Network Extension.

### Test Plan: SIP Disabled + Developer Mode

System Extension developer mode (`systemextensionsctl developer on`) allows loading unnotarized extensions, but requires SIP to be disabled first.

**Steps on andrewmac-mini:**

1. **Disable SIP** (requires Recovery Mode):
   - Reboot holding power button → "Options" → Recovery Mode
   - Terminal: `csrutil disable`
   - Reboot back to normal system
2. **Enable System Extension developer mode**:
   ```bash
   systemextensionsctl developer on
   ```
3. **Deploy the built extension**:
   - Copy `NetRewirePacketTunnel.systemextension` bundle to andrewmac-mini
   - Deploy App and Extension together
   - Manually allow the extension in System Settings → General → Login Items & Extensions
4. **Verify**:
   ```bash
   systemextensionsctl list
   # Confirm extension is loaded
   nc -v smtp.example.com 25
   # Confirm SMTP connection is tunneled
   ```

### Known Risks

- `__TEXT,__entitlements` section is a legacy approach (pre-macOS 10.12); modern macOS reads entitlements from the CMS signature blob. SIP disabled may cause the system to fall back to checking this section, but this is undocumented
- Disabling SIP reduces system security
- Even if the extension loads successfully, `neagent` may still check for a provisioning profile
- This approach is for local macOS testing only; not suitable for distribution

## Architecture

- **macOS Client**: Network Extension that intercepts TCP port 25 traffic and encapsulates it for tunneling
- **Ubuntu Server**: Receives encapsulated packets, forwards them to the public internet with NAT, and returns responses
- **Protocol**: Simple length-prefixed packet encapsulation over TCP

## Project Structure

```
net-rewire/
├── macos/
│   ├── NetRewireApp/                 # macOS app project
│   └── NetRewirePacketTunnel/        # Network Extension target
│       ├── PacketTunnelProvider.h/m  # Core tunnel logic
│       ├── pktparse.c/h              # C packet parser
│       ├── pktparse_test.c           # Unit tests
│       ├── Info.plist                # Extension configuration
│       └── NetRewirePacketTunnel.entitlements
├── ubuntu/
│   ├── tunnel_server.c               # Ubuntu tunnel server
│   ├── setup-vpn-forward.sh          # Server setup script
│   └── persist-iptables.sh           # iptables persistence
├── Makefile                          # Build system
└── README.md                         # This file
```

## Quick Start

### Ubuntu Server Setup

1. **Install dependencies and build:**
   ```bash
   make ubuntu-setup
   ```

2. **Run the tunnel server:**
   ```bash
   make ubuntu-run
   ```

### macOS Client Setup

1. **Open the Xcode project:**
   ```bash
   open macos/NetRewireApp.xcodeproj
   ```

2. **Configure code signing:**
   - Set your development team for both targets
   - Ensure Network Extension capability is enabled

3. **Build and run:**
   - Build the Network Extension target
   - Run the main app and click "Start VPN"

## Detailed Setup

### Ubuntu Server Configuration

The Ubuntu server performs the following:

1. **Creates TUN device** (`tun0`) with IP `10.8.0.1`
2. **Enables IP forwarding** for packet routing
3. **Configures iptables rules** for:
   - Forwarding TCP port 25 traffic from VPN to public interface
   - NAT (MASQUERADE) for outgoing connections
   - Accepting established/related connections back

**Manual setup commands:**
```bash
# Enable IP forwarding
sudo sysctl -w net.ipv4.ip_forward=1

# Create TUN device
sudo ip tuntap add dev tun0 mode tun
sudo ip addr add 10.8.0.1/24 dev tun0
sudo ip link set tun0 up

# Configure iptables
sudo iptables -A FORWARD -i tun0 -o eth0 -p tcp --dport 25 -j ACCEPT
sudo iptables -A FORWARD -i eth0 -o tun0 -m state --state ESTABLISHED,RELATED -j ACCEPT
sudo iptables -t nat -A POSTROUTING -o eth0 -p tcp --dport 25 -j MASQUERADE

# Persist rules
sudo netfilter-persistent save
```

### macOS Network Extension

The Network Extension:

1. **Intercepts all outbound packets** via `NEPacketTunnelProvider`
2. **Filters for TCP port 25** using fast C packet parser
3. **Encapsulates SMTP packets** and sends to Ubuntu server
4. **Re-injects non-SMTP traffic** back to host stack
5. **Handles return packets** from server and injects them to host

## Testing

### Packet Parser Tests
```bash
make test
```

### End-to-End Testing

1. **Start Ubuntu server:**
   ```bash
   make ubuntu-run
   ```

2. **Start macOS VPN:**
   - Run the app and click "Start VPN"

3. **Test SMTP connection:**
   ```bash
   nc -v smtp.gmail.com 25
   ```

4. **Monitor traffic:**
   ```bash
   # On Ubuntu - watch outgoing SMTP traffic
   sudo tcpdump -ni eth0 tcp port 25

   # On Ubuntu - watch VPN traffic
   sudo tcpdump -ni tun0

   # On macOS - watch tunnel traffic
   sudo tcpdump -ni utunX
   ```

### Verification Commands

**Check Ubuntu configuration:**
```bash
# Verify IP forwarding
cat /proc/sys/net/ipv4/ip_forward

# Check iptables rules
sudo iptables -L FORWARD -n -v
sudo iptables -t nat -L POSTROUTING -n -v

# Monitor packet counters
watch -n 1 'sudo iptables -L FORWARD -n -v && echo --- && sudo iptables -t nat -L POSTROUTING -n -v'
```

**Check macOS VPN status:**
```bash
# View Network Extension logs
log stream --predicate 'subsystem == "com.apple.NetworkExtension"'

# Check VPN connections
scutil --nc list
```

## Security Considerations

- **TLS Encryption**: Currently uses plain TCP - add TLS for production
- **Authentication**: Add client certificate authentication
- **Rate Limiting**: Implement to prevent abuse
- **Firewall Rules**: Restrict tunnel port to trusted clients
- **Logging**: Monitor for suspicious activity

## Performance

- **C Packet Parser**: Fast L3/L4 parsing in C for performance
- **Batch Processing**: Multiple packets processed in single read/write operations
- **Non-blocking I/O**: Async packet handling to prevent bottlenecks

## Troubleshooting

### Common Issues

1. **VPN won't start:**
   - Check code signing and entitlements
   - Verify Network Extension capability is enabled

2. **No traffic forwarded:**
   - Verify Ubuntu server is running
   - Check iptables rules and counters
   - Confirm IP forwarding is enabled

3. **Connection timeouts:**
   - Check firewall rules on Ubuntu
   - Verify routing and NAT configuration

### Debug Logs

**macOS:**
```bash
log stream --predicate 'subsystem contains "com.apple.NetworkExtension"'
```

**Ubuntu:**
```bash
# Run server with debug output
sudo ./ubuntu/tunnel_server

# Monitor system logs
sudo tail -f /var/log/syslog | grep -i tun
```

## Development

### Building

```bash
# Build all components
make all

# Run tests
make test

# Clean build artifacts
make clean
```

### Xcode Development

1. Open `macos/NetRewireApp.xcodeproj`
2. Set development team for both targets
3. Enable Network Extension capability
4. Build and run the Network Extension target

## License

This project is for educational and authorized security testing purposes only.