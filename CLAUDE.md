# Net-Rewire — SMTP-aware TCP proxy for outbound + inbound mail

**Architecture:** macOS daemon (C) + Ubuntu tunnel server (C) + iptables inbound forwarding + Mailcow.

## 1. Data flow

```
                              OUTBOUND
  Mailcow → daemon:2525 ──Tailscale──> VPS:tunnel_server:12345 ──> Internet MX

                              INBOUND
  Internet:25/587/465/143/993 ──> VPS:iptables DNAT ──Tailscale──> Mailcow
```

## 2. Components

### 2.1 macOS daemon (`macos/NetRewireDaemon/main.c`)

- Listens on `0.0.0.0:2525`, accepts SMTP from Mailcow Postfix
- Interactive SMTP state machine: fake 220/250 for EHLO/MAIL FROM, real RCPT TO → MX lookup
- MX resolution via `res_query()`, connects to VPS tunnel server
- Frame protocol: 4-byte BE length + payload
- Logs to `/tmp/daemon.log`, requires root, linked with `-lresolv`

### 2.2 Ubuntu tunnel server (`ubuntu/tunnel_server.c`)

- Listens on port 12345 (framed protocol), accepts daemon connections
- `CONNECT <host> <port>` → `getaddrinfo()` with AF_INET → relay bidirectionally
- Multi-threaded, detaches each client

### 2.3 Inbound forwarding (`ubuntu/inbound-forward.sh`)

- iptables DNAT on VPS public interface: ports 25/143/465/587/993 → Mailcow Tailscale IP
- Excludes Tailscale CGNAT range (100.64.0.0/10) — Tailscale clients talk directly
- Forward + MASQUERADE for return path
- Persisted via `netfilter-persistent`

### 2.4 Mailcow integration

- `docker-compose.yml`: `extra_hosts: host.docker.internal:host-gateway`
- `main.cf`: `relayhost = [host.docker.internal]:2525`, `smtp_host_lookup = native`
- DKIM: 1024-bit RSA, selector `dkim`, single TXT record
- SPF: `v=spf1 a mx ip4:<vps-ipv4> ip6:<vps-ipv6> -all`
- DMARC: `v=DMARC1; p=quarantine; adkim=s; aspf=s`

## 3. Build & deploy

```bash
# macOS daemon
make daemon
sudo macos/NetRewireDaemon/net-rewire-daemon <vps-tailscale-ip>

# VPS (one-shot deploy)
make vps-setup
scp ubuntu/* root@<vps>:~/net-rewire/
ssh root@<vps> 'cd net-rewire && MAILCOW_TS_IP=<mailcow-tailscale-ip> bash setup.sh'
```

## 4. VPS setup details

`ubuntu/setup.sh` does:
1. Install build-essential + iptables-persistent
2. Compile and install tunnel_server → `/usr/local/bin/`
3. Install systemd service `net-rewire-tunnel-server`
4. Apply inbound iptables DNAT rules
5. Enable IP forwarding + persist rules

`ubuntu/inbound-forward.sh` supports: `apply | remove | show`

## 5. Debugging

```bash
# Daemon log
tail -f /tmp/daemon.log

# VPS iptables
ssh root@<vps> 'MAILCOW_TS_IP=<ip> bash inbound-forward.sh show'

# VPS tunnel server
ssh root@<vps> 'systemctl status net-rewire-tunnel-server'
ssh root@<vps> 'journalctl -u net-rewire-tunnel-server -f'

# Test outbound SMTP from macOS
python3 -c "
import smtplib
s = smtplib.SMTP('127.0.0.1', 2525, timeout=60)
s.set_debuglevel(1)
s.sendmail('user@dtype.info', ['target@gmail.com'], 'test')
s.quit()
"

# Test inbound from external host
telnet <vps-public-ip> 25
# Should see: 220 mail.dtype.info ESMTP Postcow
```

## 6. Key design decisions

- **SMTP proxy, not packet tunnel**: Postfix connects directly via TCP. No Network Extension.
- **Fake SMTP for EHLO/MAIL FROM**: Postfix gets immediate 250; real MX responses discarded.
- **AF_INET on tunnel server**: IPv4-only for SPF alignment.
- **1024-bit DKIM**: Single TXT record, no split-ordering bug.
- **iptables for inbound**: Preserves original sender IP (DNAT, not proxy), kernel-level performance.
- **Tailscale CGNAT exclusion**: `! 100.64.0.0/10` — Tailscale clients reach Mailcow directly.
