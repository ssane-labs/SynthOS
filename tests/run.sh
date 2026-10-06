#!/bin/sh
# Boots a serial-console SynthOS image in QEMU, types commands and checks the output.
# usage: tests/run.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
qemu=${QEMU:-qemu-system-i386}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$image" "$work/disk.img"

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
EOF

awk 'NR % 2 == 1' "$work/cases" > "$work/input"
echo "shutdown" >> "$work/input"

timeout 60 "$qemu" -drive format=raw,file="$work/disk.img" -m 64 -display none -serial stdio \
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

if [ "$fail" -ne 0 ]; then
    echo "--- output ---"
    cat "$work/output"
fi
echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
