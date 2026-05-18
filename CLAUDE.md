# Net-Rewire — SMTP-aware TCP proxy for outbound mail

**Architecture:** macOS daemon (C) + Ubuntu tunnel server (C) + Mailcow Postfix relay.

Mailcow Postfix → daemon (port 2525) → Tailscale → Ubuntu VPS → destination MX.

No Network Extension, no packet capture, no UTUN — just an SMTP-aware TCP proxy that parses the envelope, resolves MX records, and tunnels through a lightweight relay server.

---

## 1. Data flow

```
Mailcow (Postfix)                 macOS daemon                  Ubuntu VPS                Internet
  relayhost ──TCP:2525──>  parse RCPT TO, MX lookup  ──TCP:12345──>  connect_to_dest()  ──>  gmail-smtp-in.l.google.com:25
                           replay SMTP handshake                  relay bytes bidirectionally
```

## 2. Components

### 2.1 macOS daemon (`macos/NetRewireDaemon/main.c`)

- Listens on `0.0.0.0:2525`, accepts SMTP from Mailcow Postfix
- **Interactive SMTP state machine**:
  1. Send fake `220` banner
  2. Read `EHLO` → send fake `250-xxx` multi-line response
  3. Read `MAIL FROM` → send fake `250 2.1.0 Ok`
  4. Read `RCPT TO` → extract recipient domain (e.g. `gmail.com`)
  5. **MX lookup** via `res_query()` — lowest-preference MX record
  6. Connect to Ubuntu VPS (Tailscale IP), send `CONNECT <mx>:25` frame
  7. Discard real MX `220` banner
  8. Replay `EHLO` to MX, **discard** response (Postfix already got fake 250)
  9. Replay `MAIL FROM` to MX, **discard** response
  10. Replay `RCPT TO` to MX, **forward** response to Postfix
  11. **Relay mode** — bidirectional byte-for-byte relay between Postfix and MX
- **Frame protocol**: 4-byte big-endian length prefix + payload
- Logs to `/tmp/daemon.log` (unbuffered)
- Requires root (`sudo`), linked with `-lresolv`

### 2.2 Ubuntu tunnel server (`ubuntu/tunnel_server.c`)

- Listens on port 12345, accepts daemon connections
- Each connection: reads `CONNECT <host> <port>` frame, connects to destination via `getaddrinfo()` (AF_INET only — IPv4 forced for SPF alignment)
- Relays bidirectionally: client → destination (raw TCP), destination → client (framed)
- Multi-threaded, detaches each client

### 2.3 Mailcow integration

- `docker-compose.yml`: `extra_hosts: host.docker.internal:host-gateway`
- `main.cf`: `relayhost = [host.docker.internal]:2525`, `smtp_host_lookup = native`
- DKIM: 1024-bit RSA key, selector `dkim`, single TXT record (fits 255-char limit)
- SPF: `v=spf1 a mx ip4:160.251.141.121 ip6:2400:8500:2002:2951::/64 -all`
- DMARC: `v=DMARC1; p=quarantine; adkim=s; aspf=s`

## 3. DNS (Google Cloud DNS, zone `dtype-info`)

| Record | Value |
|--------|-------|
| SPF | `v=spf1 a mx ip4:160.251.141.121 ip6:2400:8500:2002:2951::/64 -all` |
| DKIM | `v=DKIM1; k=rsa; p=MIGfMA0GCSq...` (1024-bit, single TXT, ≤255 chars) |
| DMARC | `v=DMARC1; p=quarantine; adkim=s; aspf=s` |

## 4. Build & deploy

```bash
# macOS daemon
make daemon
sudo macos/NetRewireDaemon/net-rewire-daemon <ubuntu-tailscale-ip>

# Ubuntu tunnel server
make ubuntu/tunnel_server
scp ubuntu/tunnel_server root@<vps>:~/
ssh root@<vps> "gcc -Wall -O2 -o tunnel_server tunnel_server.c -lpthread && sudo systemctl restart tunnel-server"
```

## 5. Debugging

```bash
# Daemon log
tail -f /tmp/daemon.log

# Test SMTP through daemon
python3 -c "
import smtplib; from email.mime.text import MIMEText
msg = MIMEText('test')
msg['From'] = 'saintway@dtype.info'
msg['To'] = 'saintway2025@gmail.com'
s = smtplib.SMTP('100.124.238.64', 2525, timeout=60)
s.set_debuglevel(1)
s.sendmail(msg['From'], [msg['To']], msg.as_string())
s.quit()
"

# Check DNS
dig txt dtype.info @8.8.8.8 +short
dig txt dkim._domainkey.dtype.info @8.8.8.8 +short

# Mailcow rspamd DKIM signing check
docker logs mailcowdockerized-rspamd-mailcow-1 | grep DKIM_SIGNED
```

## 6. Key design decisions

- **SMTP proxy, not packet tunnel**: Postfix connects directly to daemon via TCP. No PF rdr, no UTUN, no Network Extension.
- **Fake SMTP responses for EHLO/MAIL FROM**: Postfix gets immediate 250 responses, real MX responses discarded. Only RCPT TO response is forwarded.
- **AF_INET on tunnel server**: Forces IPv4 to align with SPF `ip4:` authorization.
- **1024-bit DKIM**: Fits in single TXT record (234 chars). 2048-bit would need 2 records causing random-ordering verification failures.
