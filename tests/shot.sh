#!/bin/sh
# usage: tests/shot.sh IMAGE OUT.png [seconds] [keys...]   (a key 'sleep:N' pauses N seconds)
img=$1; out=$2; wait=${3:-2}; shift 3 2>/dev/null
sock=$(mktemp -u /tmp/synthos-mon.XXXXXX)
${QEMU:-qemu-system-x86_64} -drive format=raw,file="$img",snapshot=on -m 256 ${QEMU_EXTRA} -display none -monitor unix:$sock,server,nowait &
pid=$!
sleep "$wait"
for k in "$@"; do
    case $k in sleep:*) sleep "${k#sleep:}"; continue ;; esac
    printf 'sendkey %s\n' "$k" | socat - unix-connect:$sock >/dev/null 2>&1
    sleep 0.08
done
[ $# -gt 0 ] && sleep ${AFTER:-1}
printf 'screendump %s.ppm\n' "$out" | socat - unix-connect:$sock >/dev/null 2>&1
sleep 0.5
kill $pid 2>/dev/null
python3 -c "from PIL import Image; Image.open('$out.ppm').save('$out')" && rm -f "$out.ppm"
