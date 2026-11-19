#!/bin/sh
set -eu
cd "$(dirname "$0")"
ipv4() {
    case "$1" in ''|*[!0-9.]*) return 1 ;; esac
    printf '%s\n' "$1" | awk -F. 'NF != 4 {exit 1} {for (i=1;i<=4;i++) if ($i !~ /^(0|[1-9][0-9]*)$/ || length($i)>3 || $i+0>255) exit 1}'
}
case "$#" in
    1) endpoint=$1; public=$1 ;;
    2|3) endpoint=$1; public=$2 ;;
    *) echo 'Usage: sh configure.sh PUBLIC_IPV4 | DOMAIN PUBLIC_IPV4 [BOUND_IPV4]' >&2; exit 2 ;;
esac
ipv4 "$public" || { echo 'Invalid public IPv4 address.' >&2; exit 2; }
if ipv4 "$endpoint"; then
    case "$endpoint" in
        0.*|10.*|127.*|169.254.*|172.1[6-9].*|172.2[0-9].*|172.3[01].*|192.168.*|100.6[4-9].*|100.[7-9][0-9].*|100.1[01][0-9].*|100.12[0-7].*)
            echo 'An IP certificate requires a publicly reachable IPv4 address.' >&2; exit 2 ;;
    esac
    [ "${endpoint%%.*}" -lt 224 ] || { echo 'Use a public unicast IPv4 address.' >&2; exit 2; }
    ip_certificate=yes
else
    case "$endpoint" in
        ''|*[!a-zA-Z0-9.-]*|.*|*.|*..*|-*|*.-*|*-.*|*-) echo 'Use a DNS hostname without a scheme or port.' >&2; exit 2 ;;
        *.*) ;;
        *) echo 'Use a fully qualified DNS hostname.' >&2; exit 2 ;;
    esac
    ip_certificate=no
fi
bound=${3:-$(ip -4 route get "$public" | awk '{for (i=1;i<NF;i++) if ($i=="src") {print $(i+1); exit}}')}
ipv4 "$bound" || { echo 'Pass the IPv4 address assigned to the VPS network interface as the third argument.' >&2; exit 2; }
if ! ip -o -4 addr show | awk '{print $4}' | cut -d/ -f1 | grep -Fxq "$bound"; then
    echo 'The bound IPv4 address must exist on this VPS.' >&2
    exit 2
fi
umask 077
host_key=$(openssl rand -hex 32)
turn_secret=$(openssl rand -hex 32)
if ! mkdir settings; then
    echo 'Configuration was not created. Keep existing settings and keys when restarting.' >&2
    exit 1
fi
cat > settings/signal.env <<EOF
NODE_ENV=production
EVP_SIGNAL_TRUST_PROXY=1
EVP_SIGNAL_HOST=127.0.0.1
EVP_SIGNAL_PORT=8787
EVP_HOST_KEY=$host_key
EVP_TURN_SECRET=$turn_secret
EVP_STUN_URLS=stun:$endpoint:3478
EVP_TURN_URLS=turn:$endpoint:3478?transport=udp,turn:$endpoint:3478?transport=tcp
EOF
if [ "$ip_certificate" = yes ]; then
    cat > settings/Caddyfile <<EOF
{
    default_sni $endpoint
}

$endpoint {
    tls {
        issuer acme {
            dir https://acme-v02.api.letsencrypt.org/directory
            profile shortlived
            disable_tlsalpn_challenge
        }
    }
    reverse_proxy 127.0.0.1:8787
}
EOF
else
    cat > settings/Caddyfile <<EOF
$endpoint {
    reverse_proxy 127.0.0.1:8787
}
EOF
fi
advertised=$public
if [ "$bound" != "$public" ]; then advertised="$public/$bound"; fi
cat > settings/turnserver.conf <<EOF
listening-ip=$bound
relay-ip=$bound
external-ip=$advertised
realm=$endpoint
listening-port=3478
min-port=52000
max-port=52100
fingerprint
use-auth-secret
static-auth-secret=$turn_secret
no-tls
no-dtls
no-cli
no-multicast-peers
total-quota=48
user-quota=8
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
EOF
# The parent directory is owner-only. The containers bind-mount these individual files.
chmod 644 settings/Caddyfile settings/turnserver.conf
printf 'Configured wss://%s/ (TURN %s on %s).\n' "$endpoint" "$public" "$bound"
echo 'Start with: docker compose pull && docker compose up -d'
