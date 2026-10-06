# SynthOS — notes for Claude

Read `INSTRUCTIONS.md` first (Russian, the full guide), then `docs/ARCHITECTURE.md`.

## Commands

- `make` — build `build/vga/synthos.img` (own bootloader + kernel + ramdisk)
- `make test` — serial-console build, boots in QEMU, runs `tests/run.sh` (must stay at 0 failures)
- `make run` / `make run-serial` — boot in QEMU
- `tests/shot.sh <img> <out.png> <wait-seconds> <qemu keys...>` — VGA screenshot after typing keys

## Rules

- Everything is from scratch: no Linux, no libc in the kernel, no GRUB. Hardware code lives only in `arch/`.
- Portable kernel talks to hardware only through `include/synth/hal.h`.
- Extend through registration macros: `COMMAND`, `SETTING`, `BINFMT`, `FS_TYPE` (linker sections).
- Commands print via `sh_printf`/`sh_puts` to `sh->out`, colors only via theme roles (`sh_role`).
- Errors are negative `E_*` codes. C11 (gnu11), 4 spaces, very few comments, `module_action` names.
- Build must be warning-free; add a test case to `tests/run.sh` for new behavior.
- User-facing docs are in Russian; in-OS text is English (the VGA font is CP437).
