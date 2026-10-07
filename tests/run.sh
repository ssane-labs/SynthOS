#!/bin/sh
# Boots a serial-console SynthOS image in QEMU, types commands and checks the output.
# usage: tests/run.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
qemu=${QEMU:-qemu-system-i386}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$image" "$work/disk.img"
dd if=/dev/zero of="$work/fat.img" bs=1k count=1440 2>/dev/null
mkfs.fat -n SYNTHTEST "$work/fat.img" >/dev/null
echo "hello from fat" > "$work/note.txt"
mcopy -i "$work/fat.img" "$work/note.txt" "::/Long Name Note.txt"

# each case: a command line, then the text expected somewhere in the output
cat > "$work/cases" <<'EOF'
echo hello world
hello world
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
winhello one 7; echo win=$?
win=0
winhello 9 > /tmp/w.txt; echo st=$?; grep -c arg /tmp/w.txt
st=9
winhello x | grep pi=
pi=3.14159 e=2.71828 big=1.234568e+04
winfiles | tail -n 2
cwd=C:\Windows\Temp\wtest files=3
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
lsusb
QEMU USB Keyboard
cat '/mnt/hdb/Long Name Note.txt'
hello from fat
mkdir '/mnt/hdb/My Folder'; echo saved > '/mnt/hdb/My Folder/Mixed Case.txt'; cat '/mnt/hdb/my folder/mixed case.txt'
saved
mv '/mnt/hdb/Long Name Note.txt' /mnt/hdb/n.txt; cat /mnt/hdb/n.txt | tr a-z A-Z
HELLO FROM FAT
EOF

awk 'NR % 2 == 1' "$work/cases" > "$work/input"
echo "shutdown" >> "$work/input"

timeout 60 "$qemu" -drive format=raw,file="$work/disk.img" -drive format=raw,file="$work/fat.img" -m 64 \
    -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -display none -serial stdio \
    < "$work/input" > "$work/output" 2>&1

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
