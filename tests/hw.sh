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
    if [ "$KERNEL64" = 1 ] && [ "$cpu" = 486 ]; then
        echo "  skip  $config (64-bit kernel)"
        continue
    fi
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
    [ "$cpu" = 486 ] || [ "$NO_WIN32" = 1 ] || check "$config windows" "$first" "Hello from Windows"
    [ "$cpu" = smp ] && check "$config processors" "$first" "Processors: 4 cores"
    check "$config persistence" "$second" "$tag"
done
# Sound: Intel HD Audio (the controller of real PCs, with Realtek and other codecs) plays a test tone; QEMU writes
# what reaches the codec into a WAV file: 440 Hz for half a second, then silence.
cp "$image" "$work/disk.img"
out=$(EXTRA="-audiodev wav,id=snd,path=$work/tone.wav -device intel-hda -device hda-duplex,audiodev=snd" \
      boot bios ide any "sound test 440 500")
check "sound: HD Audio found" "$out" "Sound: QEMU HD Audio"
tone=$(python3 -I -c '
import struct, sys, wave
w = wave.open(sys.argv[1])
rate, data = w.getframerate(), w.readframes(w.getnframes())
left = struct.unpack("<%dh" % (len(data) // 2), data)[0::2]
loud = [i for i, v in enumerate(left) if abs(v) > 1000]
span = (loud[-1] - loud[0]) / rate if loud else 0
ups = sum(1 for a, b in zip(left[loud[0]:loud[-1]], left[loud[0] + 1:loud[-1] + 1]) if a < 0 <= b) if loud else 0
print("tone %d Hz %.1f s" % (round(ups / span) if span else 0, span))' "$work/tone.wav" 2>&1)
check "sound: tone played" "$tone" "tone 440 Hz 0.5 s"

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
    check "install starts from the disk" "$out" "system on hdb at sector 231424 (installed)"
    mv "$work/target.img" "$work/disk.img"
    EXTRA=""
    out=$(boot uefi ide any "dmesg | grep disk:")
    check "install boots without the stick" "$out" "starting Boot"
    check "install boots without the stick" "$out" "(installed)"
    # Installing from Windows (tools/windows/install.ps1): the loader sits in the Windows EFI partition behind shim,
    # the system in a partition cut from C:. The first start comes from the boot entry bcdedit adds (here the
    # UEFI Shell's startup.nsh), after that from the entry the loader adds itself.
    cp "$ovmf_vars" "$work/vars.fd"
    at=$(python3 -I tests/winsim.py "$image" "$work/disk.img")
    out=$(EXTRA="-nic none" boot uefi ide any "dmesg | grep disk:")
    check "install from Windows starts" "$out" "${at#system } (installed)"
    mdel -i "$work/disk.img@@$((2048 * 512))" ::/startup.nsh
    out=$(EXTRA="-nic none" boot uefi ide any "cat /home/readme.txt | head -n 1")
    check "install from Windows is in the boot menu" "$out" "\\EFI\\SynthOS\\"
    check "install from Windows boots again" "$out" "Shell ready"
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

# Secure Boot: the firmware starts only a loader signed with a key in its db, and that loader starts only the kernel
# it was sealed with. OVMF's test key ("snakeoil") stands in for the key a user enrolls.
sb_code=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd
sb_vars=/usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd
sb_key=/usr/share/ovmf/PkKek-1-snakeoil.key
sb_cert=/usr/share/ovmf/PkKek-1-snakeoil.pem
secure_boot() {
    cp "$sb_vars" "$work/sbvars.fd"
    rm -f "$work/sb.txt"
    timeout 60 qemu-system-x86_64 -machine q35,smm=on -global driver=cfi.pflash01,property=secure,value=on \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$sb_code" -drive if=pflash,format=raw,unit=1,file="$work/sbvars.fd" \
        -drive format=raw,file="$work/disk.img" -m 256 -display none -serial file:"$work/sb.txt" < /dev/null > /dev/null 2>&1
    tr -d '\r' < "$work/sb.txt"
}
if [ -f "$sb_code" ] && [ -f "$sb_vars" ] && [ -f "$sb_key" ] && python3 -c 'import cryptography' 2>/dev/null; then
    esp="$work/disk.img@@$((8192 * 512))"
    cp "$image" "$work/disk.img"
    mcopy -o -i "$esp" ::/EFI/BOOT/grubx64.efi "$work/own.efi" 2>/dev/null || mcopy -o -i "$esp" ::/EFI/BOOT/BOOTX64.EFI "$work/own.efi"
    mcopy -o -i "$esp" "$work/own.efi" ::/EFI/BOOT/BOOTX64.EFI
    mcopy -o -i "$esp" ::/SYNTHOS/KERNEL.BIN "$work/kernel.bin"
    check "secure boot: loader not enrolled" "$(secure_boot)" "Access Denied"
    python3 -I tools/sbsign.py --key "$sb_key" --pass snakeoil --cert "$sb_cert" "$work/own.efi" "$work/signed.efi"
    mcopy -o -i "$esp" "$work/signed.efi" ::/EFI/BOOT/BOOTX64.EFI
    check "secure boot: signed loader" "$(secure_boot)" "Shell ready"
    python3 -c "import sys; d = bytearray(open(sys.argv[1], 'rb').read()); d[len(d) // 2] ^= 0xff; open(sys.argv[1], 'wb').write(d)" \
        "$work/kernel.bin"
    mcopy -o -i "$esp" "$work/kernel.bin" ::/SYNTHOS/KERNEL.BIN
    check "secure boot: changed kernel" "$(secure_boot)" "is not the kernel this loader was signed with"
else
    echo "  skip  secure boot (no OVMF Secure Boot firmware or python3-cryptography)"
fi
# Secure Boot with the keys PCs ship with (Microsoft's): shim starts, does not know the SynthOS key yet and opens
# MokManager, where the key is enrolled from the stick; after the restart the system comes up.
ms_code=/usr/share/OVMF/OVMF_CODE_4M.ms.fd
ms_vars=/usr/share/OVMF/OVMF_VARS_4M.ms.fd
if [ -f "$ms_code" ] && [ -f "$ms_vars" ] && mdir -i "$image@@$((8192 * 512))" ::/EFI/BOOT/grubx64.efi > /dev/null 2>&1; then
    cp "$image" "$work/disk.img"
    cp "$ms_vars" "$work/msvars.fd"
    out=$(python3 -I tests/mok.py "$ms_code" "$work/msvars.fd" "$work/disk.img")
    check "secure boot: shim and MokManager" "$out" "Shell ready"
else
    echo "  skip  secure boot with shim (no OVMF with Microsoft keys or no shim in the image)"
fi

echo "hardware tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
