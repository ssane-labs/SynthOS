#!/bin/sh
# Regenerates rootfs/etc/certs/mozilla.pem from the Mozilla CA list shipped by the host's ca-certificates.
src=${1:-/usr/share/ca-certificates/mozilla}
out=rootfs/etc/certs/mozilla.pem
{
    echo "Mozilla CA root certificates (Mozilla Public License 2.0), trusted by SynthOS for HTTPS."
    echo "Add your own roots as .pem/.crt files in /home/certs and run 'certs reload'."
    for f in "$src"/*.crt; do
        echo
        echo "# $(basename "$f" .crt)"
        sed -n '/-----BEGIN CERTIFICATE-----/,/-----END CERTIFICATE-----/p' "$f"
    done
} > "$out"
echo "$(grep -c 'BEGIN CERTIFICATE' "$out") roots written to $out"
