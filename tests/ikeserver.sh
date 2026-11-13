#!/bin/sh
# An IKEv2 server (strongSwan, userspace ESP through kernel-libipsec) for the ikev2 tests.
# ikeserver.sh WORK PORT: listens on UDP PORT (IKE and NAT-T both, so the client starts on it), gives
# addresses from 10.77.0.0/24 and serves http://10.66.0.1:8089/ inside the tunnel, which answers with the
# client's address (so a reply that did not come through the tunnel shows).
# Logins: EAP-MSCHAPv2 test/secret with the server's certificate (CA in WORK/ikeca.pem, id vpn-server),
# or the pre-shared key presharedkey123 with id client@test, whose ESP SA the server rekeys every 10 seconds.
set -e
W=$1
PORT=$2
mkdir -p $W/etc/swanctl/x509 $W/etc/swanctl/x509ca $W/etc/swanctl/private $W/run
cd $W
openssl ecparam -name prime256v1 -genkey -noout -out ca.key 2>/dev/null
openssl req -x509 -new -key ca.key -days 30 -subj "/CN=Test IKE CA" -out etc/swanctl/x509ca/ca.pem 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out etc/swanctl/private/server.pem 2>/dev/null
openssl req -new -key etc/swanctl/private/server.pem -subj "/CN=vpn-server" -out server.csr 2>/dev/null
printf "subjectAltName=DNS:vpn-server\nextendedKeyUsage=serverAuth\n" > server.ext
openssl x509 -req -in server.csr -CA etc/swanctl/x509ca/ca.pem -CAkey ca.key -CAcreateserial -days 30 \
    -extfile server.ext -out etc/swanctl/x509/server.pem 2>/dev/null
rm ca.key server.csr
cp etc/swanctl/x509ca/ca.pem ikeca.pem
cat > strongswan.conf <<C
charon {
    # only what the test needs: plugins such as forecast or connmark would touch the host's other tunnels
    load = random nonce openssl pem pkcs1 pkcs8 x509 pubkey curve25519 aes sha1 sha2 md4 des hmac gcm kernel-libipsec kernel-netlink socket-default vici eap-identity eap-mschapv2 attr
    port = $((PORT + 1))
    port_nat_t = $PORT
    piddir = $W/run
    plugins {
        vici { socket = unix://$W/run/charon.vici }
        kernel-libipsec { load = yes }
    }
    filelog { log { path = $W/server.log
                    default = 1
                    ike = 2
                    cfg = 2 } }
    syslog { daemon { default = -1 } }
    journal { default = -1 }
}
swanctl { socket = unix://$W/run/charon.vici }
C
cat > etc/swanctl/swanctl.conf <<C
connections {
    eap {
        version = 2
        pools = pool4
        proposals = aes256-sha256-x25519, aes128-sha256-ecp256, aes256gcm16-prfsha256-x25519
        send_cert = always
        local { auth = pubkey
                certs = server.pem
                id = vpn-server }
        remote { auth = eap-mschapv2
                 eap_id = %any }
        children { net { local_ts = 10.66.0.1/32
                         esp_proposals = aes256gcm16, aes256-sha256 } }
    }
    psk {
        version = 2
        pools = pool4
        proposals = aes256-sha256-x25519, aes128-sha256-ecp256
        local { auth = psk
                id = vpn-server }
        remote { auth = psk
                 id = client@test }
        children { net { local_ts = 10.66.0.1/32
                         rekey_time = 10s
                         rand_time = 0s
                         life_time = 120s
                         esp_proposals = aes128-sha256 } }
    }
}
pools { pool4 { addrs = 10.77.0.0/24
                dns = 10.66.0.1 } }
secrets {
    eap-test { id = test
               secret = secret }
    ike-psk { id-1 = client@test
              id-2 = vpn-server
              secret = presharedkey123 }
}
C
ip addr add 10.66.0.1/32 dev lo 2>/dev/null || true
export STRONGSWAN_CONF=$W/strongswan.conf
/usr/sbin/charon-systemd > $W/charon.out 2>&1 &
echo $! > $W/run/pid
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S $W/run/charon.vici ] && break; sleep 0.5; done
swanctl --load-all --file $W/etc/swanctl/swanctl.conf > $W/load.out 2>&1
python3 -I -c '
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("hello through ikev2 from %s (%s)\n" % (self.client_address[0], self.path)).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
http.server.HTTPServer(("10.66.0.1", 8089), H).serve_forever()
' > /dev/null 2>&1 &
echo $! > $W/run/http.pid
