#!/bin/sh
# Boots the serial image on several emulated machines (BIOS/UEFI, IDE/AHCI/NVMe/USB, i486)
# and checks that the shell works, a Windows program runs and /home survives a reboot.
# usage: tests/hw.sh build/serial/synthos.img

image=${1:-build/serial/synthos.img}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ovmf=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do
    [ -f "$f" ] && ovmf=$f && break
done

disk_args() {
    case $1 in
    ide) echo "-drive format=raw,file=$work/disk.img" ;;
    ahci) echo "-M q35 -drive format=raw,file=$work/disk.img,if=none,id=d -device ide-hd,drive=d,bus=ide.0" ;;
    nvme) echo "-drive format=raw,file=$work/disk.img,if=none,id=d -device nvme,serial=synth,drive=d,bootindex=0" ;;
    usb) echo "-drive format=raw,file=$work/disk.img,if=none,id=d -device piix3-usb-uhci -device usb-storage,drive=d,bootindex=0" ;;
    esac
}

boot() {
    # boot FIRMWARE DISK CPU COMMANDS...
    fw=$1 disk=$2 cpu=$3
    shift 3
    qemu=qemu-system-x86_64
    extra=""
    [ "$fw" = uefi ] && extra="-bios $ovmf"
    [ "$cpu" = 486 ] && qemu=qemu-system-i386 && extra="$extra -cpu 486"
    (sleep 12; for c in "$@"; do printf '%s\n' "$c"; done; echo shutdown) |
        timeout 60 $qemu $extra $(disk_args "$disk") -m 128 -display none -serial stdio 2>&1 | tr -d '\r'
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

for config in bios:ide:any bios:ahci:any bios:nvme:any bios:usb:any bios:ide:486 uefi:ide:any uefi:ahci:any uefi:nvme:any; do
    fw=${config%%:*}
    rest=${config#*:}
    disk=${rest%%:*}
    cpu=${rest#*:}
    if [ "$fw" = uefi ] && [ -z "$ovmf" ]; then
        echo "  skip  $config (no OVMF firmware)"
        continue
    fi
    cp "$image" "$work/disk.img"
    tag="saved-$fw-$disk"
    first=$(boot "$fw" "$disk" "$cpu" "echo $tag > /home/hw.txt" "calc 6*7" "winhello hw")
    second=$(boot "$fw" "$disk" "$cpu" "cat /home/hw.txt")
    check "$config shell" "$first" "42"
    [ "$cpu" = 486 ] || check "$config windows" "$first" "Hello from Windows"
    check "$config persistence" "$second" "$tag"
done
echo "hardware tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
