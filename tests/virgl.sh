#!/bin/sh
# Boots the serial-console image with a 3D virtio-gpu (virgl) and checks that the host renders: the "virgl"
# command clears a texture to blue and draws a red triangle on the host's graphics card, then reads it back; the
# Direct3D 11 test program (d3d64) then draws through the host and must read back what the software pipeline gives.
# Without a graphics card on the host, Mesa's llvmpipe renders in a virtual X display (Xvfb).
# usage: tests/virgl.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
qemu=${QEMU:-qemu-system-x86_64}
run=""
if [ -z "$DISPLAY" ]; then
    command -v xvfb-run >/dev/null || { echo "virgl: needs a display or xvfb-run"; exit 1; }
    run="xvfb-run -a -s -screen\ 0\ 1280x1024x24"
fi
out=$(mktemp)
trap '[ -n "$VIRGL_LOG" ] && cp "$out" "$VIRGL_LOG"; rm -f "$out"' EXIT
eval $run python3 -I - "$qemu" "$image" > "$out" 2>&1 <<'PY'
import subprocess, sys, time
qemu, image = sys.argv[1], sys.argv[2]
p = subprocess.Popen([qemu, '-drive', f'format=raw,file={image},snapshot=on', '-m', '512', '-vga', 'none',
                      '-device', 'virtio-vga-gl', '-display', 'sdl,gl=on', '-serial', 'stdio', '-smp', '1'],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import os, select
buf = b''
def wait(text, seconds):
    global buf
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.5)
        if r:
            chunk = os.read(p.stdout.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            if text.encode() in buf:
                return True
    return False
ok = wait('Shell ready', 120)
if ok:
    time.sleep(1)
    p.stdin.write(b'virgl\r'); p.stdin.flush()
    ok = wait('fences=', 60) or wait('virgl:', 1)
    p.stdin.write(b'd3d64\r'); p.stdin.flush()
    wait(' so=', 120)
    p.stdin.write(b'hostgpu\r'); p.stdin.flush()
    wait('in software', 30)
p.kill()
sys.stdout.write(buf.decode('utf-8', 'replace'))
PY
line=$(grep -a "virgl clear=" "$out" | tr -d '\r')
d3d=$(grep -a "^d3d11 level" "$out" | tr -d '\r')
host=$(grep -a "^host gpu:" "$out" | tr -d '\r')
grep -a "capset\|virgl\|^d3d11\|^host gpu\|^gd:" "$out" | tr -d '\r' | grep -v "^root@"
rc=0
case "$line" in
*"clear=ff0000ff middle=ffff0000 corner=ff0000ff"*) echo "virgl: ok" ;;
*) echo "virgl: FAIL"; rc=1 ;;
esac
case "$d3d" in
*"map=1 samples=2048 left=ff8040 right=0000ff present=-1 get=1 deferred=1 drawn=40ff00 so=1"*) echo "virgl d3d11: ok" ;;
*) echo "virgl d3d11: FAIL"; rc=1 ;;
esac
case "$host" in
*"virgl, 0 draws"*|"") echo "virgl d3d11 on the host: FAIL"; rc=1 ;;
*) echo "virgl d3d11 on the host: ok" ;;
esac
exit $rc
