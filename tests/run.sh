#!/bin/sh
# Boots a serial-console SynthOS image in QEMU, types commands and checks the output.
# usage: tests/run.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
qemu=${QEMU:-qemu-system-x86_64}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$image" "$work/disk.img"
dd if=/dev/zero of="$work/fat.img" bs=1k count=1440 2>/dev/null
mkfs.fat -n SYNTHTEST "$work/fat.img" >/dev/null
echo "hello from fat" > "$work/note.txt"
mcopy -i "$work/fat.img" "$work/note.txt" "::/Long Name Note.txt"
cat > "$work/page.html" <<'EOF'
<html><head><title>Test &amp; Page</title><script>document.write("no")</script></head><body>
<h1>Hello</h1><p>A <b>bold</b> word and a <a href="next.html">link</a>.</p>
<ul><li>first</li><li>second</li></ul><ol start="3"><li>third</li></ol>
<div hidden>invisible</div><p style="display: none">gone</p>
<form action="/find"><input name="q" placeholder="Query"><input type="submit" value="Go"></form>
<pre>  keep   spaces</pre><p>Caf&eacute; &mdash; &#x41;&#66;</p></body></html>
EOF
mcopy -i "$work/fat.img" "$work/page.html" ::/page.html
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); z.writestr('pkg/readme.txt','zipped hello '*20); z.writestr('pkg/sub/a.txt','A'); z.close()" "$work/t.zip" &&
    mcopy -i "$work/fat.img" "$work/t.zip" ::/t.zip

if command -v 7z >/dev/null; then
    mkdir -p "$work/7z/pkg/sub"
    printf 'seven zip hello\n' > "$work/7z/pkg/hello.txt"
    seq 1 20000 > "$work/7z/pkg/sub/numbers.txt"
    (cd "$work/7z" && 7z a -m0=LZMA -ms=on ../lz.7z pkg >/dev/null && 7z a -m0=Copy ../copy.7z pkg >/dev/null &&
        7z a -mf=BCJ2 -m0=LZMA ../bcj2.7z pkg >/dev/null)
    for a in lz copy bcj2; do mcopy -i "$work/fat.img" "$work/$a.7z" ::/$a.7z; done
fi

# each case: a command line, then the text expected somewhere in the output
cat > "$work/cases" <<'EOF'
ping -c 1 10.0.2.2 | grep from
64 bytes from 10.0.2.2: icmp_seq=1
dmesg | grep formatted
sfs: formatted data0
echo hello world
hello world
imginfo /system/samples/Dune.jpg
1024x640, 1 frame, average #c8967d
imginfo /system/samples/Mist.jpg
1024x640, 1 frame, average #a7b0bd
echo "a  b" 'c $USER' $USER
a  b c $USER root
calc (2 + 3) * 7
35
let x = 6 * 7; echo x=$x
x=42
for i in 1 2 3; echo item$i; end
item3
if test 5 -gt 3; echo bigger; else echo smaller; end
bigger
func twice; echo $1$1; end; twice ab
abab
echo $(echo nested) works
nested works
seq 1 5 | sort -r | head -n 1
5
printf "%05d|%s\n" 42 ok
00042|ok
mkdir -p /tmp/t/a; touch /tmp/t/a/f.txt; ls /tmp/t/a
f.txt
echo data > /tmp/t/x.txt; cat /tmp/t/x.txt
data
echo more >> /tmp/t/x.txt; wc -l /tmp/t/x.txt
2
cp /tmp/t/x.txt /tmp/t/y.txt; mv /tmp/t/y.txt /tmp/t/z.txt; ls /tmp/t
z.txt
grep -c data /tmp/t/x.txt
1
cd /tmp/t; echo *.txt
x.txt z.txt
cd /tmp; rm -r /tmp/t; test -d /tmp/t || echo removed
removed
nosuchcommand
command not found
alias say='echo said:'; say hi
said: hi
hello
Hello from SynthOS!
fib 10
0 1 1 2 3 5 8 13 21 34
args a b
2: b
primes 1000
168 primes up to 1000
asm /home/src/hello.sasm -o /tmp/h.sx
assembled /tmp/h.sx
/tmp/h.sx
Hello from SynthOS!
dis /bin/hello.sx | grep -c sys
5
hello.com
Hello from MS-DOS land
files.com
read back: file I/O through INT 21h works
mzhello.exe; echo status=$?
status=7
files.com > /tmp/dos.txt; cat /tmp/dos.txt
DOS version reported: 5.0
theme matrix; theme current
matrix
config prompt minimal; config prompt
%{prompt.symbol}
config animations off; config animations
off
config w32headless on; config w32headless; config w32headless off
on
config animations on; wmbench
desktop is not running
wmstat 1
desktop is not running
windows pin 1
desktop is not running
widgets add clock; widgets add tasks; config widgets
clock,tasks
head -n 1 ~/.widgets
dune clock,tasks
config widgets all; widgets list | grep -c desktop
6
widgets add moon
no widget called moon
config widgets clock,weather,music; config widgets
clock,weather,music
config dockapps
Launchpad|Files|Chromium|Terminal
config clock24 off; config clockseconds on; config clockdate off; config clock24; config clock24 on
clock24 = off
config clockseconds; config clockseconds off
clockseconds = on
config timezone +03:00; config timezone; config timezone UTC
timezone = +03:00
config timezone -05:30; config timezone; config timezone 0
timezone = -05:30
config timezone +15:00
timezone: invalid value '+15:00'
a=$(date); config timezone +12:00; b=$(date); config timezone UTC; test "$a" != "$b" && echo zone-applied
zone-applied
config dnd on; config dnd; config dnd off
dnd = on
config browser chrome; config browser; config browser chromium
browser = chrome
config dockdots off; config dockbounce off; config dockdots; config dockdots on
dockdots = off
config dockbounce; config dockbounce on
dockbounce = off
config doubleclick 600; config doubleclick; config doubleclick 400
doubleclick = 600
config scrollspeed 3; config scrollspeed; config scrollspeed 1
scrollspeed = 3
config mouseprimary right; config mouseprimary; config mouseprimary left
mouseprimary = right
config keydelay 250; config keyrate 20; config keyrate; config keydelay 500
keyrate = 20
config volume 40; config mute on; config volume; config mute
volume = 40
config mute; config mute off; config volume 100
mute = on
config termstatus off; config termstatus
termstatus = off
config dockapps 'Files|/home/notes.txt|Terminal'; grep dune ~/.dock
dune Files|/home/notes.txt|Terminal
config dockapps default; config dockapps
Launchpad|Files|Chromium|Terminal
config chromeauto off; config chromeauto; config chromeauto on
off
config dockstyle side; config statusbar menubar; head -n 1 ~/.looks
dune dock=side status=menubar
config dockstyle corner; config statusbar pill; config dockstyle
corner
which ls hello
hello: /bin/hello.sx
formats | grep -c DOS
2
true && echo chain-ok
chain-ok
false || echo fallback-ok
fallback-ok
! false && echo negate-ok
negate-ok
echo /home/src/*.inc
/home/src/synth.inc
countdown 1
Lift off!
mem
Memory
date | wc -w
5
echo a:b:c:d | cut -d : -f 2,4
b:d
echo hello | tr a-z A-Z
HELLO
cal 2 2024 | tail -n 1
 25 26 27 28 29
whoami
root
winpath 'C:\Windows\System32\kernel32.dll' 'C:\Program Files\App' 'c:\users\bob\Desktop'
/system/win32/kernel32.dll
winpath /home/a.txt /tmp /apps/x
C:\Users\root\a.txt
winpath 'C:\HOME\SRC\HELLO.SASM'
/home/src/hello.sasm
shortcut /apps/wintest.exe 'Win Test' info; shortcut -l '/home/Desktop/Win Test.lnk' | grep target
target: C:\apps\wintest.exe
shortcut -l '/home/Desktop/Win Test.lnk' | grep arguments
arguments: info
winhello one 7; echo win=$?
win=0
winhello 9 > /tmp/w.txt; echo st=$?; grep -c arg /tmp/w.txt
st=9
winhello x | grep pi=
pi=3.14159 e=2.71828 big=1.234568e+04
winfiles | grep cwd=
cwd=C:\Temp\wtest files=3
winfiles | grep "file dialog"
file dialog hr=0 options=20 item=C:\Temp\wtest
winfiles | grep "open dialog"
open dialog without desktop=0
winfiles | grep stream
stream write=1 read=0
winfiles | grep "file operation"
file operation hr=0 aborted=0 moved=1 left=0
wincpp | grep caught
caught: boom
wincpp | grep area | head -n 1
square area 9.00
echo 2 3.5 0 | wincalc | grep count
count=2 sum=5.50 mean=2.750
wincalc < /dev/null | grep sscanf
sscanf: 12 30 name
windll | head -n 1
hello from greet.dll, 2+3=5, counter=42
wintest crash; echo crash=$?
crash=5
wintest raise | grep filter
filter saw 0xe0001234
wintest system | grep -c Windows
2
wintest | grep build
Windows 10.0 build 19045
wintest compat | grep logfontw
logfontw=92 face=ok ncm=ok
wintest compat | grep lstrcmpW
lstrcmpW null=1,1,1
wintest compat | grep section
section read-only=ok write-dup=denied write-view=denied
wintest compat | grep power
power=ok oleacc=ok
wintest compat | grep mapped
mapped view=bbaa
wintest compat | grep image
image code=20 data=8 protected=2
wintest compat | grep object
object type=Section self=handle
wintest compat | grep reparent
reparent loop=refused move=ok children=2
wintest compat | grep gdi
gdi region=2 box=0,0,20,20 and=5,5,10,10 dc=ok world=identity current=ok blend=ok 80007f
wintest compat | grep certs
certs roots=many disallowed=0 collection=live sha1=found usage=any
wintest compat | grep registered
registered wait=ok hits=1 timeouts=ok unregister=ok
wintest shortcut | grep shortcut
shortcut saved=ok path=C:\Windows\notepad.exe args=--flag "a b"
wintest shortcut | grep misc
misc muldiv=11,-11 vartime=46311 2026-10-16 12:00 url=ok:dl.google.com:8443/chrome/install
wintest font | grep collection
collection hr=0 families=1 found=yes glyph=1
wintest font | grep bounds
glyph bounds=2,-14,13,1 center=255 outside=0
winipc | tail -n 1
sent ok, reply: child got 'hello', child exit 0
winevt | grep -c "ok,"
4
winipc2
duplicate ok/ok, child a exit 0, child b exit 0
ls C:/Windows/System32 | grep -c dll
kernel32.dll
cd 'C:\Program Files'; pwd; cd
/apps
launch | grep Mine
Clear the field without hitting a mine
launch paint
the desktop is not running
desktop
no graphics mode available
displays
no displays (text console only)
useradd -p secret bob Bob Builder
user bob created, home /home/bob
users | grep Builder
Bob Builder
su bob; echo I am $USER in $PWD; su
I am bob in /home/bob
passwd -p topsecret bob
password for bob updated
userdel bob
user bob deleted
winmem | grep -c verified
1
mem | grep Paging
Paging  PAE
winthread | head -n 1
threads done: total=20000 counter=20000 code=5000
winthread | grep preemption
preemption: ok
winthread | grep -c ok
2
winthread | grep semaphore
semaphore: got=3 waiter=7
winthread | grep condition
condition variable: consumed=5
wingdi | head -n 2 | tail -n 1
red rect=100
wingdi | grep ellipse
ellipse round
wingdi | grep line=
line=64
wingdi | grep dib
dib pixel=ff0000
wingdi | grep section
section dib=123456
config logserial on; config logserial; config logserial off
on
w32ps
no Windows programs are running
winhello one 7 > /tmp/w.txt; schedstat | grep -c "switches.*waits"
1
config w32precise | head -n 1
w32precise = on
prof start; calc 6 * 7; prof stop | grep -c "prof: .* samples"
1
config w32errors on; config w32errors | head -n 1
w32errors = on
config w32errors off; config w32trace | head -n 1
w32trace = off
wingdi | grep extent
text extent ok, ink ok
wingdi | grep blit
blit red=100
wingdi | grep stretch
stretch red=25
wingdi | grep bpp
bitmap 64x64 32bpp
winwindow
needs the SynthOS desktop
windialog | head -n 1
strings: Greeter / Hello from a string table
windialog | grep resource
dialog resource: found
windialog | tail -n 1
dialog needs the SynthOS desktop
winsock | head -n 1
winsock 2.2 SynthOS sockets
winsock | grep tcp
tcp: echo: ping
winsock | grep udp
udp: datagram from 127.0.0.1
winsock | grep getaddrinfo
getaddrinfo: 127.0.0.1 port 80
winsock | grep refused
refused: pending=1 ready=1 error=10061
ifconfig | grep inet
inet 10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2
ping -c 1 10.0.2.2 | grep from
64 bytes from 10.0.2.2: icmp_seq=1
ping -c 1 localhost | grep loss
1 packets sent, 1 received
lsusb
QEMU USB Keyboard
hello64 a b | grep longjmp
longjmp returned 42
hello64 | grep 'pointer size'
pointer size 8, long long 123456789012345, hex fedcba9876
hello64 | grep threads
threads 2000, tls 5 6 7
hello64 | grep sockets
sockets: SynthOS sockets 127.0.0.1 port 80, select 1 set 1
hello64 | grep 'window class'
window class: proc same, extra 16
hello64u a | grep math
math 1.4142 0.8415 3.162e+20
hello64u a | grep sockets
sockets: SynthOS sockets 127.0.0.1 port 80, select 1 set 1
hello64 > /tmp/h64.txt; echo rc64=$?; grep -c . /tmp/h64.txt
rc64=7
hello64 | grep math; winhello after64 | grep 'arg 1'
arg 1: after64
cpp64
~g3 ~v1 ~g2 ~g1 | caught 'deep 5' alive=0
cpp64 | grep map
caught 500, map: apple=1 fig=2 pear=1
cpp64 | grep rethrown
inner 42, rethrown 42
cat '/mnt/hdb/Long Name Note.txt'
hello from fat
mkdir '/mnt/hdb/My Folder'; echo saved > '/mnt/hdb/My Folder/Mixed Case.txt'; cat '/mnt/hdb/my folder/mixed case.txt'
saved
mv '/mnt/hdb/Long Name Note.txt' /mnt/hdb/n.txt; cat /mnt/hdb/n.txt | tr a-z A-Z
HELLO FROM FAT
browse /mnt/hdb/page.html | grep link
A bold word and a link[1].
unzip -l /mnt/hdb/t.zip
pkg/sub/a.txt
unzip -q -d /tmp/z /mnt/hdb/t.zip; cat /tmp/z/pkg/sub/a.txt /tmp/z/pkg/readme.txt
Azipped hello zipped
pkg list
Git for Windows
seq 1 1300000 > /home/big.txt; tail -n 1 /home/big.txt
1300000
wc -c /home/big.txt; rm /home/big.txt
9288896
pkg list | grep chrome
Google Chrome (32-bit)
autostart add Notepad; autostart add !echo hi; autostart list
2  !echo hi
autostart remove 1; autostart remove 1; autostart list
nothing starts at login
install
no disk to install on
fetch | grep Mirage
SynthOS 1.0 "Mirage"
music list
2  Better Days - SynthOS Lofi
music play 3; music status
Desert Wind - SynthOS Ambient, playing
browse /mnt/hdb/page.html | grep next
   1. file:///mnt/hdb/next.html
browse -n /mnt/hdb/page.html | grep third
  3. third
browse -n /mnt/hdb/page.html | grep Query
[Query____] [Go]
browse -n /mnt/hdb/page.html | grep Caf
Café - AB
browse -n /mnt/hdb/page.html | wc
      17      23     125
EOF

# SynthFS read-ahead and cache must not serve old data after the file is rewritten and appended to
cat >> "$work/cases" <<EOF
seq 1 20000 > /home/ra.txt; sha256sum /home/ra.txt; seq 7 20006 > /home/ra.txt; tail -n 1 /home/ra.txt; echo end >> /home/ra.txt; sha256sum /home/ra.txt
$( (seq 7 20006; echo end) | sha256sum | cut -c1-64)  /home/ra.txt
EOF

apps="$(dirname "$(dirname "$image")")/rootfs/apps"
if [ -f "$apps/winbench.exe" ]; then
    cat >> "$work/cases" <<EOF
cp /apps/winbench.exe /mnt/hdb/wb.exe; cp /mnt/hdb/wb.exe /mnt/hdb/wb2.exe; sha256sum /mnt/hdb/wb2.exe
$(sha256sum "$apps/winbench.exe" | cut -c1-64)  /mnt/hdb/wb2.exe
winbench /tmp /tmp/wb.txt; grep total /tmp/wb.txt
(check 0 0 0)
EOF
fi

if command -v 7z >/dev/null; then
    cat >> "$work/cases" <<EOF
un7z /mnt/hdb/lz.7z /tmp/7a; cat /tmp/7a/pkg/hello.txt
seven zip hello
un7z /mnt/hdb/copy.7z /tmp/7b; tail -n 1 /tmp/7b/pkg/sub/numbers.txt
20000
un7z /mnt/hdb/bcj2.7z /tmp/7c; sha256sum /tmp/7c/pkg/sub/numbers.txt
$(sha256sum "$work/7z/pkg/sub/numbers.txt" | cut -c1-64)
un7z /mnt/hdb/page.html
not a 7z archive
EOF
fi

server=""
if command -v openssl >/dev/null; then
    port=$((20000 + $$ % 20000))
    mkdir -p "$work/www"
    echo "secret over tls" > "$work/www/hello.txt"
    cp rootfs/system/samples/Night.jpg "$work/www/starry-night.jpg"
    printf 'subjectAltName=IP:10.0.2.2\nbasicConstraints=CA:FALSE\n' > "$work/ext.cnf"
    openssl req -x509 -newkey rsa:2048 -sha256 -days 30 -nodes -keyout "$work/ca.key" -out "$work/ca.pem" \
        -subj "/CN=SynthOS Test CA" 2>/dev/null
    openssl ecparam -name prime256v1 -genkey -noout -out "$work/srv.key"
    openssl req -new -key "$work/srv.key" -out "$work/srv.csr" -subj "/CN=10.0.2.2" 2>/dev/null
    openssl x509 -req -in "$work/srv.csr" -CA "$work/ca.pem" -CAkey "$work/ca.key" -CAcreateserial -days 30 \
        -sha256 -extfile "$work/ext.cnf" -out "$work/srv.pem" 2>/dev/null
    mcopy -i "$work/fat.img" "$work/ca.pem" ::/test-ca.pem
    (cd "$work/www" && exec openssl s_server -accept "$port" -cert "$work/srv.pem" -key "$work/srv.key" -WWW -quiet) \
        >/dev/null 2>&1 &
    server=$!
    (cd "$work/www" && exec openssl s_server -accept "$((port + 1))" -cert "$work/srv.pem" -key "$work/srv.key" -WWW \
        -quiet -tls1_2 -cipher ECDHE-ECDSA-AES256-GCM-SHA384) >/dev/null 2>&1 &
    server="$server $!"
    cp "$work/page.html" "$work/www/page.html"
    if command -v python3 >/dev/null; then
        cat > "$work/cookie.py" <<'PYEOF'
import gzip, http.server, sys

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        if self.path == "/set":
            self.send_header("Set-Cookie", "session=abc123; Path=/; HttpOnly")
            self.send_header("Set-Cookie", "theme=dark; Max-Age=3600")
            body = "<p>set</p>"
        elif self.path == "/drop":
            self.send_header("Set-Cookie", "theme=; Max-Age=0")
            body = "<p>dropped</p>"
        elif self.path == "/gz":
            text = ("packed " * 4000 + "end of gzip\n").encode()
            data = gzip.compress(text) if "gzip" in self.headers.get("Accept-Encoding", "") else text
            if data is not text:
                self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        else:
            body = "<p>cookie=[%s]</p>" % self.headers.get("Cookie", "none")
        data = body.encode()
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass

http.server.HTTPServer(("", int(sys.argv[1])), Handler).serve_forever()
PYEOF
        python3 -I "$work/cookie.py" "$((port + 2))" >/dev/null 2>&1 &
        server="$server $!"
        cat >> "$work/cases" <<EOF
browse -n http://10.0.2.2:$((port + 2))/set; browse -n http://10.0.2.2:$((port + 2))/echo
cookie=[session=abc123; theme=dark]
browse -n http://10.0.2.2:$((port + 2))/drop; browse -n http://10.0.2.2:$((port + 2))/echo
cookie=[session=abc123]
cookies
1 cookie stored
cookies clear; browse -n http://10.0.2.2:$((port + 2))/echo
cookie=[none]
wget -O /tmp/gz.txt http://10.0.2.2:$((port + 2))/gz >/dev/null; wc -c /tmp/gz.txt; grep -c "end of gzip" /tmp/gz.txt
28012
EOF
    fi
    cat >> "$work/cases" <<EOF
wget -O - https://10.0.2.2:$port/hello.txt
certificate is not trusted
mkdir /home/certs; cp /mnt/hdb/test-ca.pem /home/certs/; certs reload | grep -c trusted; wget -O - https://10.0.2.2:$port/hello.txt
secret over tls
wallpaper https://10.0.2.2:$port/starry-night.jpg
Wallpapers/starry-night.jpg and set it as the wallpaper
config wallpaper
/Pictures/Wallpapers/starry-night.jpg
wallpaper https://10.0.2.2:$port/hello.txt
not a picture
wget -O /tmp/t.txt https://10.0.2.2:$port/hello.txt | grep TLS
TLS 1.3, AES-128-GCM, certificate for 10.0.2.2 (verified)
sha256sum /tmp/t.txt
$(sha256sum "$work/www/hello.txt" | cut -c1-64)  /tmp/t.txt
wget -O /tmp/t12.txt https://10.0.2.2:$((port + 1))/hello.txt | grep TLS; cat /tmp/t12.txt
TLS 1.2, AES-256-GCM, certificate for 10.0.2.2 (verified)
browse -n https://10.0.2.2:$port/page.html | grep bold
A bold word and a link.
cd /tmp; winnet https://10.0.2.2:$port/hello.txt | grep wininet
wininet: status 200, 16 bytes, first line 'secret over tls'
cd /tmp; winnet64 https://10.0.2.2:$port/hello.txt | grep winhttp
winhttp: status 200, 16 bytes
cd /tmp; winnet64 https://10.0.2.2:$port/hello.txt | grep urlmon
urlmon: ok, 16 bytes
EOF
fi

awk 'NR % 2 == 1' "$work/cases" > "$work/input"
echo "shutdown" >> "$work/input"

timeout 90 "$qemu" -drive format=raw,file="$work/disk.img" -drive format=raw,file="$work/fat.img" -m 64 \
    -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -nic user,model=e1000 -display none -serial stdio \
    < "$work/input" > "$work/output" 2>&1

[ -n "$server" ] && kill $server 2>/dev/null

pass=0
fail=0
while IFS= read -r command && IFS= read -r expected; do
    if grep -qF -- "$expected" "$work/output"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n        expected: %s\n' "$command" "$expected"
    fi
done < "$work/cases"

if command -v mdir >/dev/null && ! mdir -i "$work/fat.img" "::/My Folder" | grep -q "Mixed Case.txt"; then
    fail=$((fail + 1))
    echo "  FAIL  FAT image written by SynthOS is not readable on the host"
fi

if [ "$fail" -ne 0 ]; then
    echo "--- output ---"
    cat "$work/output"
fi
echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
