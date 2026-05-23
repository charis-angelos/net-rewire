# Net-Rewire Makefile
# Build system for C components

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE
LDFLAGS =

# Targets
TARGETS = ubuntu/tunnel_server macos/NetRewirePacketTunnel/pktparse_test macos/NetRewireDaemon/net-rewire-daemon

.PHONY: all clean test daemon vps-setup

all: $(TARGETS)

# macOS daemon (SMTP-aware proxy)
macos/NetRewireDaemon/net-rewire-daemon: macos/NetRewireDaemon/main.c
	$(CC) $(CFLAGS) -o $@ macos/NetRewireDaemon/main.c $(LDFLAGS) -lresolv

daemon: macos/NetRewireDaemon/net-rewire-daemon

# Ubuntu tunnel server
ubuntu/tunnel_server: ubuntu/tunnel_server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread

# Packet parser test
macos/NetRewirePacketTunnel/pktparse_test: macos/NetRewirePacketTunnel/pktparse_test.c macos/NetRewirePacketTunnel/pktparse.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Test the packet parser
test: macos/NetRewirePacketTunnel/pktparse_test
	./macos/NetRewirePacketTunnel/pktparse_test

# Clean build artifacts
clean:
	rm -f $(TARGETS)
	rm -f *.o
	rm -f macos/NetRewireDaemon/*.o

vps-setup: ubuntu/tunnel_server
	@echo "VPS build ready. Deploy with:"
	@echo "  scp ubuntu/* root@<vps>:~/net-rewire/"
	@echo "  ssh root@<vps> 'cd net-rewire && MAILCOW_TS_IP=<ip> bash setup.sh'"

help:
	@echo "Net-Rewire Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build all components"
	@echo "  daemon        - Build macOS SMTP proxy daemon"
	@echo "  test          - Run packet parser tests"
	@echo "  vps-setup     - Build tunnel server (ready for VPS deploy)"
	@echo "  clean         - Clean build artifacts"
	@echo ""
	@echo "Daemon usage:"
	@echo "  sudo macos/NetRewireDaemon/net-rewire-daemon <ubuntu-tailscale-ip>"
	@echo ""
	@echo "VPS deploy:"
	@echo "  MAILCOW_TS_IP=<ip> bash ubuntu/setup.sh"