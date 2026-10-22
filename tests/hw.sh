#!/bin/sh
# Boots the serial image on several emulated machines (BIOS/UEFI, IDE/AHCI/NVMe/USB/xHCI, i486)
# and checks that the shell works, a Windows program runs and /home survives a reboot.
# usage: tests/hw.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ovmf=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do
    [ -f "$f" ] && ovmf="-bios $f" && break
done
# split firmware keeps its boot entries in a variable store, which the install test needs
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd; do
    vars=$(echo "$f" | sed 's/CODE/VARS/')
    if [ -f "$f" ] && [ -f "$vars" ]; then
        cp "$vars" "$work/vars.fd"
        ovmf="-drive if=pflash,format=raw,readonly=on,file=$f -drive if=pflash,format=raw,file=$work/vars.fd"
        ovmf_vars=$vars
        break
    fi
done

disk_args() {
    case $1 in
    ide) echo "-drive format=raw,file=$work/disk.img" ;;
    ahci) echo "-M q35 -drive format=raw,file=$work/disk.img,if=none,id=d -device ide-hd,drive=d,bus=ide.0" ;;
    nvme) echo "-drive format=raw,file=$work/disk.img,if=none,id=d -device nvme,serial=synth,drive=d,bootindex=0" ;;
    usb) echo "-drive format=raw,file=$work/disk.img,if=none,id=d -device piix3-usb-uhci -device usb-storage,drive=d,bootindex=0" ;;
    xhci) echo "-drive format=raw,file=$work/disk.img,if=none,id=d -device qemu-xhci,id=x -device usb-storage,bus=x.0,drive=d,bootindex=0 -device usb-kbd,bus=x.0" ;;
    esac
}

boot() {
    # boot FIRMWARE DISK CPU COMMANDS... (commands are typed once the shell is ready)
    fw=$1 disk=$2 cpu=$3
    shift 3
    qemu=qemu-system-x86_64
    extra=""
    [ "$fw" = uefi ] && extra="$ovmf"
    [ "$cpu" = 486 ] && qemu=qemu-system-i386 && extra="$extra -cpu 486"
    [ "$cpu" = smp ] && extra="$extra -smp 4"
    rm -f "$work/in" "$work/out"
    mkfifo "$work/in"
    timeout 90 $qemu $extra $(disk_args "$disk") $EXTRA -m 128 -display none -serial stdio < "$work/in" > "$work/out" 2>&1 &
    pid=$!
    exec 3> "$work/in"
    tries=0
    while ! grep -q "Shell ready" "$work/out" 2>/dev/null && [ $tries -lt 240 ] && kill -0 $pid 2>/dev/null; do
        sleep 0.25
        tries=$((tries + 1))
    done
    sleep 1
    for c in "$@"; do printf '%s\n' "$c" >&3; done
    echo shutdown >&3
    wait $pid
    exec 3>&-
    tr -d '\r' < "$work/out"
}

pass=0
fail=0
check() {
    if printf '%s' "$2" | grep -qF -- "$3"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL  $1: expected '$3'"
    fi
}

for config in bios:ide:any bios:ahci:any bios:nvme:any bios:usb:any bios:xhci:any bios:ide:486 uefi:ide:any uefi:ahci:any uefi:nvme:any uefi:xhci:any bios:ahci:smp uefi:ahci:smp; do
    fw=${config%%:*}
    rest=${config#*:}
    disk=${rest%%:*}
    cpu=${rest#*:}
    if [ "$fw" = uefi ] && [ -z "$ovmf" ]; then
        echo "  skip  $config (no OVMF firmware)"
        continue
    fi
    cp "$image" "$work/disk.img"
    [ -n "$ovmf_vars" ] && cp "$ovmf_vars" "$work/vars.fd"
    tag="saved-$fw-$disk"
    first=$(boot "$fw" "$disk" "$cpu" "echo $tag > /home/hw.txt" "calc 6*7" "winhello hw")
    second=$(boot "$fw" "$disk" "$cpu" "cat /home/hw.txt")
    check "$config shell" "$first" "42"
    [ "$cpu" = 486 ] || check "$config windows" "$first" "Hello from Windows"
    [ "$cpu" = smp ] && check "$config processors" "$first" "Processors: 4 cores"
    check "$config persistence" "$second" "$tag"
done
# Installing: next to another system on a GPT disk (UEFI: the stick registers the new system in the firmware boot
# menu and starts it, then it starts alone), and over a whole disk with BIOS.
if [ -n "$ovmf_vars" ]; then
    cp "$ovmf_vars" "$work/vars.fd"
    cp "$image" "$work/disk.img"
    rm -f "$work/target.img"
    truncate -s 300M "$work/target.img"
    EXTRA="-drive format=raw,file=$work/target.img"
    out=$(boot uefi ide any "part hdb init gpt --yes" "part hdb add 100 data Windows --yes" \
        "install hdb --free 1 --size 120 --yes")
    check "install next to Windows" "$out" "SynthOS is installed on hdb"
    check "install keeps Windows" "$(python3 -I tools/gptcheck.py "$work/target.img")" "1 2048 206847 Windows"
    out=$(boot uefi ide any "dmesg | grep disk:")
    check "install registers in the boot menu" "$out" "found the new installation"
    check "install starts from the disk" "$out" "system on hdb at sector 215040 (installed)"
    mv "$work/target.img" "$work/disk.img"
    EXTRA=""
    out=$(boot uefi ide any "dmesg | grep disk:")
    check "install boots without the stick" "$out" "starting Boot"
    check "install boots without the stick" "$out" "(installed)"
else
    echo "  skip  install (no OVMF variable store)"
fi
cp "$image" "$work/disk.img"
rm -f "$work/whole.img"
truncate -s 100M "$work/whole.img"
EXTRA="-drive format=raw,file=$work/whole.img"
out=$(boot bios ide any "install hdb --whole --yes")
check "install whole disk" "$out" "SynthOS is installed on hdb"
mv "$work/whole.img" "$work/disk.img"
EXTRA=""
out=$(boot bios ide any "dmesg | grep disk:")
check "install whole disk boots" "$out" "system on hda at sector 0 (installed)"

echo "hardware tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
