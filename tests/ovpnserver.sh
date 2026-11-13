#!/bin/sh
# An OpenVPN server for the tests (needs root, /dev/net/tun and openvpn): tls-crypt, a user name and password
# (test / secret), the tunnel 10.88.0.0/24 with the host at 10.88.0.1, which serves HTTP on port
# 8088. Writes WORK/test.ovpn for the client and runs until killed.
# Usage: ovpnserver.sh WORK PORT
work=$1 port=$2
cd "$work" || exit 1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -sha256 -days 30 -nodes -keyout ca.key \
    -out ca.pem -subj "/CN=SynthOS Test VPN CA" 2>/dev/null
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout srv.key -out srv.csr \
    -subj "/CN=vpn-server" 2>/dev/null
printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n' > ext.cnf
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 -sha256 -extfile ext.cnf \
    -out srv.pem 2>/dev/null
openvpn --genkey secret tc.key >/dev/null 2>&1
printf '#!/bin/sh\n[ "$(sed -n 1p "$1")" = test ] && [ "$(sed -n 2p "$1")" = secret ]\n' > check.sh
chmod +x check.sh
{
    printf 'client\ndev tun\nproto udp\nremote 10.0.2.2 %s\nverify-x509-name vpn-server name\n<ca>\n' "$port"
    cat ca.pem
    printf '</ca>\n<tls-crypt>\n'
    cat tc.key
    printf '</tls-crypt>\n'
} > test.ovpn
echo "hello through openvpn" > hi.txt
openvpn --port "$port" --proto udp --dev tunsy --dev-type tun --ca ca.pem --cert srv.pem --key srv.key --dh none \
    --topology subnet --server 10.88.0.0 255.255.255.0 --tls-crypt tc.key \
    --verify-client-cert none --username-as-common-name --script-security 2 \
    --auth-user-pass-verify "$work/check.sh" via-file --data-ciphers AES-256-GCM:CHACHA20-POLY1305 \
    --push "dhcp-option DNS 10.88.0.1" --keepalive 10 60 --verb 3 --log "$work/server.log" &
vpn=$!
trap 'kill $vpn $web 2>/dev/null' EXIT INT TERM
for i in 1 2 3 4 5 6 7 8 9 10; do ip addr show tunsy 2>/dev/null | grep -q 10.88.0.1 && break; sleep 0.5; done
python3 -I -m http.server 8088 --bind 10.88.0.1 >/dev/null 2>&1 &
web=$!
wait $vpn
