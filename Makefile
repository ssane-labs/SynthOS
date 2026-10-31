#  SynthOS build
#
#  make            build the bootable disk image (build/vga/synthos.img, 64-bit; ARCH=i686 for old PCs)
#  make usb        the image for a USB stick (build/synthos-usb.img)
#  make run        boot it in QEMU
#  make test       build a serial-console image and run the automated tests
#  make clean      remove build output

include config.mk

BUILD    ?= build
# the 64-bit kernel by default; ARCH=i686 builds the 32-bit one for old PCs (i486 and up: build/vga32, build/serial32,
# build/synthos-usb32.img)
ARCH     ?= x86_64
X64      := $(filter x86_64,$(ARCH))
SUFFIX   := $(if $(X64),,32)
OUT      := $(BUILD)/$(if $(SERIAL),serial,vga)$(SUFFIX)
IMAGE    := $(OUT)/synthos.img

ifneq ($(X64),)
ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
CROSS    ?= x86_64-elf-
endif
W32 ?= 1
ifneq ($(W32),1)
CONFIG_WIN32 := 0
CONFIG_DEFINES := $(subst -DCONFIG_WIN32=1,-DCONFIG_WIN32=0,$(CONFIG_DEFINES))
endif
else ifneq ($(shell command -v i686-elf-gcc 2>/dev/null),)
CROSS    ?= i686-elf-
endif

CC        = $(CROSS)gcc
LD        = $(CROSS)ld
OBJCOPY   = $(CROSS)objcopy
NASM     ?= nasm
HOSTCC   ?= cc
QEMU     ?= qemu-system-x86_64
QEMU_SOUND ?= -audiodev none,id=snd -device intel-hda -device hda-duplex,audiodev=snd

ifneq ($(X64),)
ARCH_FLAGS := $(if $(CROSS),,-m64) -march=x86-64 -mno-red-zone -mcmodel=small -mno-sse -mno-mmx -mno-80387
else
ARCH_FLAGS := $(if $(CROSS),,-m32) -march=i486 -mtune=i686 -mno-sse -mno-mmx -mno-80387
endif
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pic -fno-pie -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-override-init -Wno-unused-parameter \
          -Iinclude $(CONFIG_DEFINES) $(if $(SERIAL),-DCONFIG_SERIAL_CONSOLE)
# the 64-bit kernel runs at 0xb0100000, above what -mcmodel=small reaches with absolute addresses: position
# independent code keeps every access RIP-relative
ifneq ($(X64),)
CFLAGS := $(filter-out -fno-pic -fno-pie,$(CFLAGS)) -fpie
endif
LDFLAGS := $(if $(CROSS),,$(if $(X64),-m elf_x86_64 -z max-page-size=4096 -no-pie --no-relax,-m elf_i386)) -nostdlib \
           -T arch/x86/$(if $(X64),64/)linker.ld $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx && echo --no-warn-rwx-segments)
HOSTCFLAGS := -std=gnu11 -O2 -Wall -Wextra -Iinclude -DSYNTH_TOOL

KERNEL_SRC := $(shell find kernel -name '*.c') $(wildcard arch/x86/*.c)
ifneq ($(X64),)
KERNEL_SRC := $(filter-out arch/x86/cpu.c arch/x86/paging.c arch/x86/libgcc.c,$(KERNEL_SRC)) \
              $(wildcard arch/x86/64/*.c)
endif
ifeq ($(CONFIG_DOS),0)
KERNEL_SRC := $(filter-out kernel/dos/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_SXVM),0)
KERNEL_SRC := $(filter-out kernel/vm/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_WIN32),0)
KERNEL_SRC := $(filter-out kernel/win32/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_GUI),0)
KERNEL_SRC := $(filter-out kernel/gui/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_EDITOR),0)
KERNEL_SRC := $(filter-out kernel/apps/edit.c,$(KERNEL_SRC))
endif
KERNEL_ASM := arch/x86/entry.asm arch/x86/isr.asm arch/x86/win32.asm arch/x86/long.asm
ifneq ($(X64),)
KERNEL_ASM := arch/x86/64/entry.asm arch/x86/64/isr.asm $(if $(filter 1,$(W32)),arch/x86/64/win32.asm arch/x86/64/w64.asm)
endif
NASM_OBJ   := $(if $(X64),elf64,elf32)
KERNEL_OBJ := $(KERNEL_SRC:%.c=$(OUT)/obj/%.o) $(KERNEL_ASM:%.asm=$(OUT)/obj/%.o) $(OUT)/obj/biosblob.o \
              $(if $(and $(X64),$(filter 1,$(W32))),$(OUT)/obj/x32/fmt.o)

SX_SRC   := $(wildcard programs/sx/*.sasm)
SX_BIN   := $(SX_SRC:programs/sx/%.sasm=$(BUILD)/rootfs/bin/%.sx)
DOS_SRC  := $(wildcard programs/dos/*.asm)
EXE_SRC  := $(wildcard programs/dos/exe/*.asm)
DOS_BIN  := $(DOS_SRC:programs/dos/%.asm=$(BUILD)/rootfs/dos/%.com) $(EXE_SRC:programs/dos/exe/%.asm=$(BUILD)/rootfs/dos/%.exe)
W32CC    ?= $(shell command -v i686-w64-mingw32-gcc 2>/dev/null)
W32CXX   ?= $(if $(W32CC),$(W32CC:%gcc=%g++))
W32_SRC  := $(wildcard programs/win32/*.c programs/win32/*.cpp)
W32_DLL  := $(patsubst programs/win32/dll/%.c,$(BUILD)/rootfs/apps/%.dll,$(wildcard programs/win32/dll/*.c))
W32_BIN  := $(patsubst programs/win32/%,$(BUILD)/rootfs/apps/%.exe,$(basename $(W32_SRC))) $(W32_DLL)
MSVC_SRC := $(wildcard programs/win32/msvc/*.cpp)
W32_BIN  += $(MSVC_SRC:programs/win32/msvc/%.cpp=$(BUILD)/rootfs/apps/%.exe)
CLANG    ?= $(shell command -v clang 2>/dev/null)
LLDLINK  ?= $(shell command -v lld-link 2>/dev/null)
DLLTOOL  ?= $(shell command -v llvm-dlltool 2>/dev/null)
W64CC    ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
W64CXX   ?= $(shell command -v x86_64-w64-mingw32-g++ 2>/dev/null)
W64_SRC  := $(wildcard programs/win64/*.c programs/win64/*.cpp)
W32_BIN  += $(patsubst programs/win64/%,$(BUILD)/rootfs/apps/%.exe,$(basename $(W64_SRC))) $(BUILD)/rootfs/apps/hello64u.exe
ROOTFS_SRC := $(shell find rootfs -type f)

TOOLS := $(BUILD)/tools/mkrd $(BUILD)/tools/sasm
SASM_SRC := tools/sasm.c kernel/vm/asm.c kernel/vm/opcodes.c kernel/lib/strutil.c kernel/lib/fmt.c

.DELETE_ON_ERROR:
.PHONY: all run run-serial run-uefi test test-hw test-crypto clean tools programs usb windows
all: $(IMAGE)

ESP_LBA     := 8192
ESP_SECTORS := 24576
SYSTEM_SECTORS := $(shell echo $$(( $(ESP_LBA) + $(ESP_SECTORS) )))

# The desktop image carries Chromium (its own 32-bit Windows snapshot, BSD-licensed) as a bundle after the system
# area; the first boot unpacks it into /home/apps/chromium. BUNDLE=0 or no network leaves it out.
CHROMIUM_REV ?= 1721499
BUNDLE       ?= $(if $(SERIAL),0,1)
BUNDLE_FILE  := $(if $(filter 1,$(BUNDLE)),$(BUILD)/cache/chromium-$(CHROMIUM_REV).bundle)
BUNDLE_SECTORS = $(if $(BUNDLE_FILE),$(shell echo $$(( $$(stat -c %s $(BUNDLE_FILE) 2>/dev/null || echo 0) / 512 ))),0)
DISK_SECTORS   = $(if $(filter 0,$(BUNDLE_SECTORS)),$(IMAGE_SECTORS),$(shell echo $$(( $(SYSTEM_SECTORS) + $(BUNDLE_SECTORS) + 4194304 ))))

$(BUILD)/cache/chrome-win-$(CHROMIUM_REV).zip:
	@mkdir -p $(@D)
	@echo "  FETCH   Chromium $(CHROMIUM_REV) (32-bit Windows snapshot)"
	@curl -fsSL --retry 3 -o $@.part https://commondatastorage.googleapis.com/chromium-browser-snapshots/Win/$(CHROMIUM_REV)/chrome-win.zip \
	    && mv $@.part $@ || (rm -f $@.part; echo "  SKIP    no network: the image goes without Chromium"; : > $@)

$(BUILD)/cache/chromium-$(CHROMIUM_REV).bundle: $(BUILD)/cache/chrome-win-$(CHROMIUM_REV).zip tools/mkbundle.py
	@if [ -s $< ]; then python3 -I tools/mkbundle.py $< $@ "Chromium" chrome-win/ _tests.exe; else : > $@; fi
EFICC       ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
HAVE_ESP    := $(and $(shell command -v mkfs.fat 2>/dev/null),$(shell command -v mcopy 2>/dev/null))

$(IMAGE): $(OUT)/stage1.bin $(OUT)/stage2.bin $(OUT)/kernel.bin $(if $(HAVE_ESP),$(OUT)/esp.img) $(BUNDLE_FILE)
	@echo "  IMAGE   $@"
	@test $$(( $$(wc -c < $(OUT)/kernel.bin) / 512 + 17 )) -le $(ESP_LBA) || (echo "kernel too large for the disk layout"; exit 1)
	@cat $(OUT)/stage1.bin $(OUT)/stage2.bin $(OUT)/kernel.bin > $@
	@dd if=/dev/zero of=$@ bs=512 count=0 seek=$(DISK_SECTORS) 2>/dev/null
	@if [ -s "$(BUNDLE_FILE)" ]; then dd if=$(BUNDLE_FILE) of=$@ bs=1M oflag=seek_bytes seek=$$(($(SYSTEM_SECTORS) * 512)) conv=notrunc,sparse 2>/dev/null; fi
ifneq ($(HAVE_ESP),)
	@dd if=$(OUT)/esp.img of=$@ bs=512 seek=$(ESP_LBA) conv=notrunc 2>/dev/null
	@echo "  ready   $@ ($$(( $$(wc -c < $(OUT)/kernel.bin) / 1024 )) KB kernel, BIOS + UEFI)"
else
	@echo "  ready   $@ ($$(( $$(wc -c < $(OUT)/kernel.bin) / 1024 )) KB kernel, BIOS only: install dosfstools and mtools for UEFI)"
endif

# Secure Boot: the loader on the EFI partition carries the SHA-256 of its kernel and is signed with the key in
# $(SB_DIR) (made on the first build, kept across "make clean"); SYNTHOS.CER next to the kernel is the certificate
# to enroll in the firmware (docs/INSTALL.md). SB_KEY/SB_CERT pick another key, SB=0 leaves the loader unsigned.
SB_DIR      ?= keys
SB_KEY      ?= $(SB_DIR)/db.key
SB_CERT     ?= $(SB_DIR)/db.crt
SB          ?= $(if $(and $(shell command -v openssl 2>/dev/null),$(shell python3 -c 'import cryptography' 2>/dev/null && echo y)),1,0)

$(SB_DIR)/db.key:
	@mkdir -p $(@D)
	@echo "  KEYGEN  $(SB_DIR) (Secure Boot signing key, keep it private)"
	@openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -subj "/CN=SynthOS Secure Boot $$(hostname)/" \
	    -keyout $(SB_DIR)/db.key -out $(SB_DIR)/db.crt 2>/dev/null

$(OUT)/BOOTX64.EFI: $(BUILD)/efi/BOOTX64.EFI $(OUT)/kernel.bin tools/sbsign.py $(if $(filter 1,$(SB)),$(SB_KEY))
	@echo "  SIGN    $@$(if $(filter 1,$(SB)),, (sealed, not signed))"
	@python3 -I tools/sbsign.py $(if $(filter 1,$(SB)),--key $(SB_KEY) --cert $(SB_CERT)) --seal $(OUT)/kernel.bin $< $@
	@$(if $(filter 1,$(SB)),openssl x509 -in $(SB_CERT) -outform DER -out $(OUT)/SYNTHOS.CER,rm -f $(OUT)/SYNTHOS.CER)

# shim: the first stage that firmware with Microsoft's keys starts under Secure Boot (Ubuntu's build, signed by
# Microsoft, BSD-licensed). It starts the SynthOS loader (grubx64.efi next to it) once the SynthOS key is in its
# MOK list; MokManager (mmx64.efi) adds the key on the first start, so the firmware settings stay as they are.
# SHIM=0 or no network: the loader alone is BOOTX64.EFI.
SHIM_DEB    := shim-signed_1.58+15.8-0ubuntu1_amd64.deb
SHIM_SHA256 := ba9b5d80e5d886c30664f2bebfb5c2fcce3b9b40f16fc46cba49c19a91c8059c
SHIM        ?= 1
SHIM_DIR    := $(BUILD)/cache/shim
SHIM_FILES  := $(if $(filter 1,$(SHIM)),$(SHIM_DIR)/stamp)

$(SHIM_DIR)/stamp:
	@mkdir -p $(@D)
	@echo "  FETCH   shim (Microsoft-signed first stage for Secure Boot)"
	@if curl -fsSL --retry 3 -o $(@D)/$(SHIM_DEB) http://archive.ubuntu.com/ubuntu/pool/main/s/shim-signed/$(SHIM_DEB) \
	    && echo "$(SHIM_SHA256)  $(@D)/$(SHIM_DEB)" | sha256sum -c --quiet - \
	    && python3 -I tools/debfiles.py $(@D)/$(SHIM_DEB) $(@D) usr/lib/shim/shimx64.efi.dualsigned=shimx64.efi \
	       usr/lib/shim/mmx64.efi usr/share/doc/shim-signed/copyright=SHIM.TXT; then : > $@; \
	else rm -f $(@D)/shimx64.efi; echo "  SKIP    no shim: Secure Boot needs the key in the firmware"; : > $@; fi

$(OUT)/esp.img: $(OUT)/kernel.bin $(OUT)/BOOTX64.EFI $(SHIM_FILES)
	@echo "  ESP     $@"
	@rm -f $@
	@mkfs.fat -F 16 -n SYNTHOS -C $@ $$(( $(ESP_SECTORS) / 2 )) >/dev/null
	@mmd -i $@ ::/EFI ::/EFI/BOOT ::/SYNTHOS
	@if [ -s $(SHIM_DIR)/shimx64.efi ] && [ -n "$(SHIM_FILES)" ]; then \
	    mcopy -i $@ $(SHIM_DIR)/shimx64.efi ::/EFI/BOOT/BOOTX64.EFI; \
	    mcopy -i $@ $(SHIM_DIR)/mmx64.efi ::/EFI/BOOT/mmx64.efi; \
	    mcopy -i $@ $(OUT)/BOOTX64.EFI ::/EFI/BOOT/grubx64.efi; \
	    mcopy -i $@ $(SHIM_DIR)/SHIM.TXT ::/SYNTHOS/SHIM.TXT; \
	else mcopy -i $@ $(OUT)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI; fi
	@mcopy -i $@ $(OUT)/kernel.bin ::/SYNTHOS/KERNEL.BIN
	@if [ -f $(OUT)/SYNTHOS.CER ]; then mcopy -i $@ $(OUT)/SYNTHOS.CER ::/SYNTHOS/SYNTHOS.CER; fi

$(BUILD)/efi/BOOTX64.EFI: arch/x86/efi/loader.c arch/x86/efi/trampoline.asm arch/x86/efi/blob.asm
	@mkdir -p $(@D)
ifneq ($(EFICC),)
	@echo "  EFI     $@"
	@$(NASM) -f bin -o $(BUILD)/efi/trampoline.bin arch/x86/efi/trampoline.asm
	@$(NASM) -f win64 -DTRAMPOLINE_FILE='"$(BUILD)/efi/trampoline.bin"' -o $(BUILD)/efi/blob.o arch/x86/efi/blob.asm
	@$(EFICC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
	    -nostdlib -shared -Wl,--subsystem,10 -e efi_main -o $@ arch/x86/efi/loader.c $(BUILD)/efi/blob.o
	@mkdir -p arch/x86/efi/prebuilt && cp $@ arch/x86/efi/prebuilt/
else
	@echo "  COPY    arch/x86/efi/prebuilt/BOOTX64.EFI"
	@cp arch/x86/efi/prebuilt/BOOTX64.EFI $@
endif

# The partition table depends on the bundle and disk size: rebuild stage1 whenever they change.
$(OUT)/layout.txt: $(BUNDLE_FILE) FORCE
	@mkdir -p $(@D)
	@echo "$(DISK_SECTORS) $(BUNDLE_SECTORS)" | cmp -s - $@ || echo "$(DISK_SECTORS) $(BUNDLE_SECTORS)" > $@

$(OUT)/stage1.bin: arch/x86/boot/stage1.asm arch/x86/boot/layout.inc config.mk $(OUT)/layout.txt
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ -DIMAGE_SECTORS=$(DISK_SECTORS) -DBUNDLE_SECTORS=$(BUNDLE_SECTORS) $(BOOT_DEFS) -o $@ $<

$(OUT)/stage2.bin: arch/x86/boot/stage2.asm arch/x86/boot/layout.inc
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ $(BOOT_DEFS) -o $@ $<

$(OUT)/kernel.bin: $(OUT)/kernel.elf $(BUILD)/ramdisk.bin $(BUILD)/tools/mkrd
	@$(OBJCOPY) -O binary $< $(OUT)/image.bin
	@$(BUILD)/tools/mkrd --kernel $(OUT)/image.bin $(BUILD)/ramdisk.bin $@

$(OUT)/kernel.elf: $(KERNEL_OBJ) arch/x86/$(if $(X64),64/)linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJ)

$(OUT)/obj/%_fp.o: CFLAGS += $(if $(X64),-msse -msse2 -mfpmath=sse,-m80387 -mfpmath=387)
# The 64-bit kernel's Windows subsystem is x32 code (64-bit mode, 4-byte pointers and longs: the structure layouts
# 32-bit programs use), built by clang and turned into ELF64 objects. Every source gets its call gate signatures
# (tools/w32sig.py) appended; references to the few kernel functions whose pointer or size_t out-parameters differ
# go to arch/x86/64/w32abi.c, and formatting uses an x32 copy of kernel/lib/fmt.c.
ifneq ($(X64),)
X32CFLAGS := --target=x86_64-linux-gnux32 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
             -fno-pie -mno-red-zone -mcmodel=small -mno-sse -mno-mmx -mno-80387 -fno-strict-aliasing \
             -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -ftrivial-auto-var-init=zero -fms-extensions \
             -Wno-microsoft -Wall -Wextra -Wno-override-init -Wno-unused-parameter -Wno-initializer-overrides \
             -Wno-missing-braces -Wno-deprecated-non-prototype -Iinclude $(CONFIG_DEFINES) $(if $(SERIAL),-DCONFIG_SERIAL_CONSOLE)
W32SIG_CC := clang -std=gnu11 -ffreestanding -fno-builtin -fms-extensions -Iinclude $(CONFIG_DEFINES) \
             $(if $(SERIAL),-DCONFIG_SERIAL_CONSOLE)
$(OUT)/obj/kernel/win32/%_fp.o: X32CFLAGS += -msse -msse2 -mfpmath=sse -m80387
$(OUT)/obj/kernel/win32/%.o: kernel/win32/%.c tools/w32sig.py arch/x86/64/x32.syms
	@mkdir -p $(@D)
	@echo "  CC      $< (x32)"
	@python3 -I tools/w32sig.py $< $(@:.o=.sig.h) $(W32SIG_CC)
	@printf '#include "%s"\n#include "%s"\n' "$(abspath $<)" "$(abspath $(@:.o=.sig.h))" > $(@:.o=.wrap.c)
	@clang $(X32CFLAGS) -MMD -MP -MT $@ -MF $(@:.o=.d) -c $(@:.o=.wrap.c) -o $(@:.o=.x32.o)
	@objcopy -I elf32-x86-64 -O elf64-x86-64 --redefine-syms=arch/x86/64/x32.syms $(@:.o=.x32.o) $@
$(OUT)/obj/x32/fmt.o: kernel/lib/fmt.c arch/x86/64/x32.syms
	@mkdir -p $(@D)
	@echo "  CC      $< (x32)"
	@clang $(X32CFLAGS) -MMD -MP -MT $@ -MF $(@:.o=.d) -c $< -o $(@:.o=.x32.o)
	@objcopy -I elf32-x86-64 -O elf64-x86-64 --redefine-syms=arch/x86/64/x32.syms $(@:.o=.x32.o) $@
endif
$(OUT)/obj/kernel/win32/%.o: CFLAGS += -Os
# desktop code that runs on clicks, not per frame: small rather than fast (the 32-bit kernel must stay below 3.5 MiB)
GUI_SMALL := $(addprefix $(OUT)/obj/kernel/gui/,widgets.o widgetkinds.o control.o screenshot.o mission.o notify.o \
             dockpeek.o setup.o lock.o spotlight.o picker.o installed.o autostart.o wallnet.o quicklook.o)
$(OUT)/obj/kernel/gui/apps/%.o $(GUI_SMALL): CFLAGS += -Os
$(OUT)/obj/kernel/win32/crt/%.o: CFLAGS += -O2
$(OUT)/obj/kernel/win32/thread.o $(OUT)/obj/kernel/win32/process.o $(OUT)/obj/kernel/win32/files.o $(OUT)/obj/kernel/win32/pe.o: CFLAGS += -O2

$(OUT)/obj/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/%.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f $(NASM_OBJ) -o $@ $<

$(OUT)/biosthunk.bin: arch/x86/$(if $(X64),64/)biosthunk.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(OUT)/smpboot.bin: arch/x86/$(if $(X64),64/)smpboot.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(OUT)/obj/biosblob.o: arch/x86/biosblob.asm $(OUT)/biosthunk.bin $(OUT)/smpboot.bin
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f $(NASM_OBJ) -DTHUNK_FILE='"$(OUT)/biosthunk.bin"' -DSMP_FILE='"$(OUT)/smpboot.bin"' -o $@ $<

$(BUILD)/ramdisk.bin: $(BUILD)/tools/mkrd $(ROOTFS_SRC) $(SX_BIN) $(DOS_BIN) $(W32_BIN) $(wildcard programs/sx/*)
	@mkdir -p $(BUILD)/rootfs
	@cp -r rootfs/. $(BUILD)/rootfs/
	@mkdir -p $(BUILD)/rootfs/home/src
	@cp programs/sx/*.sasm programs/sx/*.inc $(BUILD)/rootfs/home/src/
	@$(BUILD)/tools/mkrd $(BUILD)/rootfs $@

$(BUILD)/rootfs/bin/%.sx: programs/sx/%.sasm programs/sx/synth.inc $(BUILD)/tools/sasm
	@mkdir -p $(@D)
	@echo "  SASM    $<"
	@$(BUILD)/tools/sasm -o $@ $<

$(BUILD)/rootfs/dos/%.com: programs/dos/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/rootfs/dos/%.exe: programs/dos/exe/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/rootfs/apps/windll.exe: $(BUILD)/rootfs/apps/greet.dll

$(BUILD)/rootfs/apps/%.dll: programs/win32/dll/%.c
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CC   $<"
	@$(W32CC) -Os -s -shared -o $@ $<
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win32/%.c
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CC   $<"
	@$(if $(wildcard programs/win32/$*.rc),$(W32CC:%gcc=%windres) -O coff -o $(BUILD)/$*.res.o programs/win32/$*.rc)
	@$(W32CC) -Os -s -o $@ $< $(filter %.dll,$^) $(if $(wildcard programs/win32/$*.rc),$(BUILD)/$*.res.o) -lgdi32 -lws2_32 -lwininet -lwinhttp -lurlmon -lcrypt32 -lole32 -lshell32 -lshlwapi -lcomdlg32 -lbcrypt -lsecur32
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

# Programs built the way Microsoft's compiler builds them (MSVC C++ exceptions), with clang and lld-link.
$(BUILD)/rootfs/apps/%.exe: programs/win32/msvc/%.cpp programs/win32/msvc/msvcrt.def
	@mkdir -p $(@D)
ifneq ($(and $(CLANG),$(LLDLINK),$(DLLTOOL)),)
	@echo "  MSVC    $<"
	@$(CLANG) --target=i686-pc-windows-msvc -O1 -fexceptions -fcxx-exceptions -c $< -o $(BUILD)/$*.obj
	@$(DLLTOOL) -k -m i386 -d programs/win32/msvc/msvcrt.def -l $(BUILD)/msvcrt.lib
	@$(LLDLINK) /nologo /entry:start /subsystem:console /nodefaultlib /out:$@ $(BUILD)/$*.obj $(BUILD)/msvcrt.lib
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/hello64u.exe: programs/win64/hello64.c
	@mkdir -p $(@D)
ifneq ($(W64CC),)
	@echo "  W64CC   $< (UCRT)"
	@$(W64CC) -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > $(BUILD)/ucrt64.specs
	@$(W64CC) -specs=$(BUILD)/ucrt64.specs -D_UCRT -O2 -s -o $@ $< -lws2_32
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/dll64.exe: $(BUILD)/rootfs/apps/tls64.dll

$(BUILD)/rootfs/apps/tls64.dll: programs/win64/dll/tls64.c
	@mkdir -p $(@D)
ifneq ($(W64CC),)
	@echo "  W64CC   $<"
	@$(W64CC) -O2 -s -shared -o $@ $<
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win64/%.c
	@mkdir -p $(@D)
ifneq ($(W64CC),)
	@echo "  W64CC   $<"
	@$(W64CC) -O2 -s -o $@ $< -lgdi32 -lws2_32 -lwininet -lwinhttp -lurlmon
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win64/%.cpp
	@mkdir -p $(@D)
ifneq ($(W64CXX),)
	@echo "  W64CXX  $<"
	@$(W64CXX) -Os -s -static -o $@ $<
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win32/%.cpp
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CXX  $<"
	@$(W32CXX) -Os -s -static -o $@ $<
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/tools/mkrd: tools/mkrd.c
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(BUILD)/tools/sasm: $(SASM_SRC) $(wildcard include/synth/*.h)
	@mkdir -p $(@D)
	@echo "  HOSTCC  tools/sasm"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $(SASM_SRC)

tools: $(TOOLS)
programs: $(SX_BIN) $(DOS_BIN) $(W32_BIN)

run: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE) -m 256 $(QEMU_SOUND) $(QEMU_FLAGS)

run-serial:
	@$(MAKE) --no-print-directory SERIAL=1
	$(QEMU) -drive format=raw,file=$(BUILD)/serial$(SUFFIX)/synthos.img -m 256 -nographic $(QEMU_FLAGS)

test:
	@$(MAKE) --no-print-directory SERIAL=1
	@$(if $(X64),KERNEL64=1 $(if $(filter 1,$(W32)),,NO_WIN32=1) )tests/run.sh $(BUILD)/serial$(SUFFIX)/synthos.img

test-crypto:
	@mkdir -p $(BUILD)/tools
	@$(HOSTCC) -O2 -Iinclude -o $(BUILD)/tools/cryptotest tests/crypto/driver.c kernel/crypto/hash.c kernel/crypto/aes.c \
	    kernel/crypto/chacha.c kernel/crypto/x25519.c kernel/crypto/pk.c kernel/lib/sha256.c kernel/lib/sha1.c kernel/lib/md5.c
	@python3 -I tests/crypto/check.py $(BUILD)/tools/cryptotest

test-hw:
	@$(MAKE) --no-print-directory SERIAL=1
	@$(if $(X64),KERNEL64=1 $(if $(filter 1,$(W32)),,NO_WIN32=1) )tests/hw.sh $(BUILD)/serial$(SUFFIX)/synthos.img

OVMF ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd))
OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd))
UEFI_FW = $(if $(OVMF),-bios $(OVMF),-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(BUILD)/uefi-vars.fd)

run-uefi: $(IMAGE)
	@[ -n "$(OVMF)" ] || [ -f $(BUILD)/uefi-vars.fd ] || cp $(subst CODE,VARS,$(OVMF_CODE)) $(BUILD)/uefi-vars.fd
	qemu-system-x86_64 $(UEFI_FW) -drive format=raw,file=$(IMAGE) -m 256 $(QEMU_SOUND) $(QEMU_FLAGS)

# the image to write to a USB stick (Rufus in DD mode, balenaEtcher or dd); the data partition grows to the stick
usb: $(IMAGE)
	@python3 -I tools/mkusb.py $(IMAGE) $(BUILD)/synthos-usb$(SUFFIX).img

# Install from Windows without a USB stick (tools/windows): the system area and the bundle, the EFI files and the
# scripts that put them next to Windows.
WIN_DIR := $(BUILD)/synthos-windows$(SUFFIX)
windows: $(IMAGE) $(OUT)/esp.img
	@echo "  WINDOWS $(WIN_DIR).zip"
	@rm -rf $(WIN_DIR) $(WIN_DIR).zip && mkdir -p $(WIN_DIR)/EFI
	@dd if=$(IMAGE) of=$(WIN_DIR)/system.img bs=512 count=$$(( $(SYSTEM_SECTORS) + $(BUNDLE_SECTORS) )) 2>/dev/null
	@cp tools/windows/*.cmd tools/windows/*.ps1 tools/windows/README.txt $(WIN_DIR)/
	@cp $(OUT)/BOOTX64.EFI $(WIN_DIR)/EFI/grubx64.efi
	@cp $(OUT)/kernel.bin $(WIN_DIR)/EFI/KERNEL.BIN
	@if [ -f $(OUT)/SYNTHOS.CER ]; then cp $(OUT)/SYNTHOS.CER $(WIN_DIR)/EFI/; fi
	@if [ -s $(SHIM_DIR)/shimx64.efi ] && [ -n "$(SHIM_FILES)" ]; then \
	    cp $(SHIM_DIR)/shimx64.efi $(SHIM_DIR)/mmx64.efi $(SHIM_DIR)/SHIM.TXT $(WIN_DIR)/EFI/; fi
	@cd $(BUILD) && python3 -I -m zipfile -c $(notdir $(WIN_DIR)).zip $(notdir $(WIN_DIR))
	@echo "  ready   $(WIN_DIR).zip ($$(( $$(stat -c %s $(WIN_DIR).zip) >> 20 )) MB)"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

FORCE:
