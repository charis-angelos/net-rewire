#!/usr/bin/env bash
# Net-Rewire inbound mail forwarding rules
# Forwards public mail ports from VPS to Mailcow over Tailscale.
#
# Fill in your Mailcow server's Tailscale IP before running:
#   MAILCOW_TS_IP=100.x.y.z bash inbound-forward.sh apply
set -euo pipefail

PUB_IF="${PUB_IF:-eth0}"
TAILSCALE_IF="${TAILSCALE_IF:-tailscale0}"
MAILCOW_TS_IP="${MAILCOW_TS_IP:?set MAILCOW_TS_IP to your Mailcow Tailscale IP}"
TAILSCALE_NET="100.64.0.0/10"

# Mail ports to forward
PORTS_TCP=(25 143 465 587 993)

cmd="${1:-show}"

show_rules() {
    echo "=== PREROUTING (DNAT) ==="
    sudo iptables -t nat -L PREROUTING -n -v --line-numbers
    echo ""
    echo "=== FORWARD ==="
    sudo iptables -L FORWARD -n -v --line-numbers
    echo ""
    echo "=== POSTROUTING (MASQUERADE) ==="
    sudo iptables -t nat -L POSTROUTING -n -v --line-numbers
}

apply_rules() {
    echo "Applying inbound mail forwarding for ${MAILCOW_TS_IP}..."

    for port in "${PORTS_TCP[@]}"; do
        # DNAT: public traffic (not from Tailscale) → Mailcow
        if ! sudo iptables -t nat -C PREROUTING -i "$PUB_IF" ! -s "$TAILSCALE_NET" \
             -p tcp --dport "$port" -j DNAT --to-destination "${MAILCOW_TS_IP}:${port}" 2>/dev/null; then
            sudo iptables -t nat -A PREROUTING -i "$PUB_IF" ! -s "$TAILSCALE_NET" \
                -p tcp --dport "$port" -j DNAT --to-destination "${MAILCOW_TS_IP}:${port}"
        fi

        # FORWARD: allow forwarding to Mailcow
        if ! sudo iptables -C FORWARD -i "$PUB_IF" -p tcp -d "$MAILCOW_TS_IP" \
             --dport "$port" -o "$TAILSCALE_IF" -j ACCEPT 2>/dev/null; then
            sudo iptables -A FORWARD -i "$PUB_IF" -p tcp -d "$MAILCOW_TS_IP" \
                --dport "$port" -o "$TAILSCALE_IF" -j ACCEPT
        fi
    done

    # MASQUERADE: source-NAT return traffic back through VPS
    if ! sudo iptables -t nat -C POSTROUTING -p tcp -d "$MAILCOW_TS_IP" \
         -m multiport --dports "$(IFS=,; echo "${PORTS_TCP[*]}")" \
         -o "$TAILSCALE_IF" -j MASQUERADE 2>/dev/null; then
        sudo iptables -t nat -A POSTROUTING -p tcp -d "$MAILCOW_TS_IP" \
            -m multiport --dports "$(IFS=,; echo "${PORTS_TCP[*]}")" \
            -o "$TAILSCALE_IF" -j MASQUERADE
    fi

    echo "Rules applied."
    show_rules
}

remove_rules() {
    echo "Removing inbound mail forwarding..."

    for port in "${PORTS_TCP[@]}"; do
        sudo iptables -t nat -D PREROUTING -i "$PUB_IF" ! -s "$TAILSCALE_NET" \
            -p tcp --dport "$port" -j DNAT --to-destination "${MAILCOW_TS_IP}:${port}" 2>/dev/null || true
        sudo iptables -D FORWARD -i "$PUB_IF" -p tcp -d "$MAILCOW_TS_IP" \
            --dport "$port" -o "$TAILSCALE_IF" -j ACCEPT 2>/dev/null || true
    done

    sudo iptables -t nat -D POSTROUTING -p tcp -d "$MAILCOW_TS_IP" \
        -m multiport --dports "$(IFS=,; echo "${PORTS_TCP[*]}")" \
        -o "$TAILSCALE_IF" -j MASQUERADE 2>/dev/null || true

    echo "Rules removed."
}

case "$cmd" in
    apply)   apply_rules ;;
    remove)  remove_rules ;;
    show)    show_rules ;;
    *)
        echo "Usage: $0 {apply|remove|show}"
        echo ""
        echo "Environment variables:"
        echo "  MAILCOW_TS_IP    Mailcow server Tailscale IP (required)"
        echo "  PUB_IF           Public interface (default: eth0)"
        echo "  TAILSCALE_IF     Tailscale interface (default: tailscale0)"
        exit 1
        ;;
esac
