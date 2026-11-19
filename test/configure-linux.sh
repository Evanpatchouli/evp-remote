#!/bin/sh
# Run with sh on Linux. Interface discovery is deterministic; certificate issuance is separate.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
cat > "$work/bin/ip" <<'EOF'
#!/bin/sh
case "$*" in
    '-4 route get '*) echo "test-route dev eth0 src 10.2.3.4" ;;
    '-o -4 addr show') echo "2: eth0 inet 10.2.3.4/24" ;;
    *) exit 1 ;;
esac
EOF
chmod +x "$work/bin/ip"
export PATH="$work/bin:$PATH"

install_fixture() {
    mkdir "$work/$1"
    cp "$root/deploy/linux/configure.sh" "$work/$1/"
    cd "$work/$1"
}
install_fixture ip
sh configure.sh 203.0.113.20 >/dev/null
grep -Fxq '    default_sni 203.0.113.20' settings/Caddyfile
grep -Fxq '            profile shortlived' settings/Caddyfile
grep -Fxq '            dir https://acme-v02.api.letsencrypt.org/directory' settings/Caddyfile
grep -Fxq 'EVP_STUN_URLS=stun:203.0.113.20:3478' settings/signal.env
grep -Fxq 'EVP_TURN_URLS=turn:203.0.113.20:3478?transport=udp,turn:203.0.113.20:3478?transport=tcp' settings/signal.env
grep -Fxq 'external-ip=203.0.113.20/10.2.3.4' settings/turnserver.conf
before=$(sha256sum settings/*)
if sh configure.sh 203.0.113.20 >/dev/null 2>&1; then exit 1; fi
test "$before" = "$(sha256sum settings/*)"

install_fixture dns
sh configure.sh remote.example.net 203.0.113.20 >/dev/null
grep -Fxq 'remote.example.net {' settings/Caddyfile
grep -Fxq 'EVP_STUN_URLS=stun:remote.example.net:3478' settings/signal.env
if grep -Fq shortlived settings/Caddyfile; then exit 1; fi
install_fixture bound
sh configure.sh remote.example.net 203.0.113.20 10.2.3.4 >/dev/null
grep -Fxq 'listening-ip=10.2.3.4' settings/turnserver.conf

install_fixture invalid
for value in remote.example.net 256.1.1.1 192.168.1.20 100.64.0.1 127.0.0.1 224.1.1.1 01.2.3.4 ''; do
    if sh configure.sh "$value" >/dev/null 2>&1; then exit 1; fi
    test ! -e settings
done
echo 'IP-only, hostname, NAT mapping, key preservation and invalid input checks passed.'
