# SynthOS — инструкция

Этот файл — главная точка входа в проект. Его достаточно прочитать (человеку или Claude в новом
чате), чтобы собрать систему, запустить её и продолжить разработку.

* Что умеет система и как выглядит — [README.md](README.md)
* Как она устроена внутри — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
* Справка внутри самой ОС — команды `help`, `more /home/readme.txt`, файлы в `/docs`

---

## 1. Текущее состояние

SynthOS 0.1.0 «Neon» — рабочая база:

* свой загрузчик для **BIOS** (MBR + stage2, LBA или CHS, карта памяти E820/E801/88h) и свой
  загрузчик для **UEFI** (`BOOTX64.EFI`) — один и тот же образ грузится на старых и новых ПК;
* 32-битное ядро x86 (от **i486** и 4–5 МБ ОЗУ), страничная память, null-страница защищена;
* консоль: VGA-текст или графический фреймбуфер UEFI, **свой шрифт 8×16 с кириллицей** (CP437/CP866),
  темы; клавиатура PS/2, таймер, RTC, COM-порт;
* диски: IDE/ATA, **AHCI (SATA)**, **NVMe**, а на BIOS-машинах — любой диск через BIOS (USB-флешки);
* файловые системы: ramfs (корень), devfs, своя дисковая **SynthFS** (`/home` сохраняется);
  структура каталогов в стиле Windows: `C:\Windows`, `C:\Program Files`, `C:\Users\<имя>`;
* оболочка с 84 командами, скриптами, пайпами, подсветкой синтаксиса и автодополнением;
* **запуск программ Windows** (32-битные консольные `.exe`/`.dll`): свой загрузчик PE и свои
  kernel32, msvcrt, user32, advapi32, shell32, ntdll — C и C++ (STL, исключения), файлы, консоль
  с цветами, DLL, MessageBox, вложенный запуск;
* виртуальная машина **SX** для своих программ + ассемблер/дизассемблер внутри ОС;
* эмулятор **MS-DOS** (8086/186) для `.COM` и `.EXE`;
* редактор `edit`, пейджер `more`, игра `snake`;
* автотесты: `make test` (63 проверки) и `make test-hw` (BIOS/UEFI × IDE/AHCI/NVMe/USB, i486).

---

## 2. Что установить на свой компьютер

Linux здесь нужен **только как среда сборки** (компилятор и ассемблер). Сама SynthOS от него не
зависит и загружается на голом железе.

### Windows 10/11 (рекомендуется через WSL)

1. В PowerShell от администратора: `wsl --install` → перезагрузка → откроется Ubuntu, задайте имя
   пользователя и пароль.
2. В терминале Ubuntu:
   ```sh
   sudo apt update
   sudo apt install -y build-essential nasm qemu-system-x86 git \
       gcc-mingw-w64-i686 g++-mingw-w64-i686 gcc-mingw-w64-x86-64 dosfstools mtools ovmf
   ```
   Обязательны только `build-essential nasm`. Остальное: `mingw-w64` — собирать примеры
   Windows-программ и UEFI-загрузчик (без него берутся готовые файлы из `prebuilt/`),
   `dosfstools mtools` — добавить в образ раздел UEFI, `qemu-system-x86` и `ovmf` — запуск в эмуляторе.
3. Скопировать репозиторий внутрь WSL (так сборка быстрее), например:
   `git clone <адрес репозитория> ~/SynthOS` или `cp -r /mnt/c/путь/SynthOS ~/SynthOS`.
4. QEMU из WSL открывает окно через WSLg (Windows 11 — из коробки). Если окна нет — установите
   QEMU для Windows (qemu.org) и запускайте образ оттуда:
   `qemu-system-i386 -drive format=raw,file=synthos.img -m 64`.

### Linux (Ubuntu / Debian / Mint)

```sh
sudo apt install -y build-essential nasm qemu-system-x86 git \
    gcc-mingw-w64-i686 g++-mingw-w64-i686 gcc-mingw-w64-x86-64 dosfstools mtools ovmf
```
(Fedora: `sudo dnf install gcc make nasm qemu-system-x86 mingw32-gcc mingw32-gcc-c++ mingw64-gcc dosfstools mtools edk2-ovmf`;
Arch: `sudo pacman -S base-devel nasm qemu-full mingw-w64-gcc dosfstools mtools edk2-ovmf`.)

### macOS

```sh
brew install i686-elf-gcc i686-elf-binutils nasm qemu mingw-w64 dosfstools mtools
```
Makefile сам найдёт кросс-компилятор `i686-elf-gcc`.

---

## 3. Сборка и запуск

```sh
cd SynthOS
make              # собрать build/vga/synthos.img (загрузочный образ диска 16 МБ, BIOS + UEFI)
make run          # запустить в QEMU как старый ПК с BIOS (окно с VGA-экраном)
make run-uefi     # запустить в QEMU как современный ПК с UEFI (графическая консоль)
make run-serial   # запустить с консолью в текущем терминале через COM-порт
make test         # собрать вариант с COM-консолью и прогнать автотесты
make test-hw      # прогнать образ на 8 разных «машинах» (BIOS/UEFI, IDE/AHCI/NVMe/USB, i486)
make clean        # удалить build/
```

Полезное:

* Выход из QEMU: команда `shutdown` внутри SynthOS (или закрыть окно; в `run-serial` — `Ctrl+A`, затем `X`).
* Файлы в `/home` хранятся в самом образе. Чтобы начать «с чистого диска» — `make clean && make`.
* Отключение подсистем для маленьких машин (`config.mk` или командная строка):
  `make CONFIG_DOS=0 CONFIG_SXVM=0 CONFIG_EDITOR=0`.
* Дополнительные флаги QEMU: `make run QEMU_FLAGS="-m 128"`.

### На реальном компьютере

1. Записать образ на флешку **побайтно** (все данные на флешке пропадут):
   * Linux/WSL: `sudo dd if=build/vga/synthos.img of=/dev/sdX bs=1M status=progress && sync`
   * Windows: Rufus → выбрать `synthos.img` → режим **DD-образ**.
2. Загрузиться с флешки через меню загрузки (обычно F12, F11, F8 или Esc). Подходит и
   **UEFI**, и **Legacy/CSM** — образ гибридный. **Secure Boot** нужно выключить
   (загрузчик не подписан).

Где сохраняется `/home`: на диске, с которого загрузилась система, если это IDE, SATA (AHCI)
или NVMe; при загрузке через BIOS — на любом диске, который видит BIOS (включая USB-флешку).
При загрузке через UEFI с USB-флешки `/home` пока живёт в памяти (драйвера USB нет).
Чужие диски система не трогает: раздел используется, только если на диске лежит именно
загрузочный образ SynthOS (сигнатура в MBR + заголовок ядра).

Требования к железу: процессор x86 от i486, 5 МБ ОЗУ (C++-программам Windows — от 6 МБ),
клавиатура PS/2 или USB-клавиатура с эмуляцией PS/2 (её дают почти все BIOS/UEFI, на ноутбуках
встроенная клавиатура обычно подключена именно так). Видео: VGA (BIOS) или любой
фреймбуфер GOP (UEFI) ниже 4 ГБ.

---

## 4. Как пользоваться (коротко)

```text
help / help ls            список команд / справка по команде
ls -l, cd, tree, find     файлы; edit файл — редактор; more файл — просмотр
cd "C:\Program Files"     пути Windows понимаются напрямую; drives, winpath — карта дисков
theme, prompt, color      оформление; config — все настройки; config save — сохранить
config codepage 866       кириллица (CP866) — работает и в VGA, и в UEFI
hello, fib 20, snake      программы SX из /bin
hello.com, box.com        программы DOS из /dos
winhello, wincpp, windll  программы Windows из /apps (= C:\Program Files)
asm файл.sasm             собрать свою программу; dis файл.sx — дизассемблировать
```

Про Windows-программы подробно — `more /docs/windows.txt` внутри ОС.

Скрипты: файл `.sh` с командами оболочки (`if … end`, `while … end`, `for x in … end`,
`func имя … end`). Стартовый скрипт пользователя — `/home/.synthrc`.
Полный синтаксис — `more /docs/shell.txt` внутри ОС.

---

## 5. Структура репозитория

```text
Makefile, config.mk       сборка и параметры (размер образа, включаемые подсистемы)
arch/x86/boot/            stage1.asm (MBR, LBA/CHS), stage2.asm (A20, E820/E801, ядро), layout.inc
arch/x86/efi/             UEFI-загрузчик: loader.c (BOOTX64.EFI), trampoline.asm (выход из long mode)
arch/x86/                 entry.asm, isr.asm, linker.ld, cpu.c (GDT/IDT/PIC, CPUID/FPU), timer.c,
                          keyboard.c, serial.c, vga.c (+ загрузка шрифта), paging.c (страничная память),
                          ata.c, ahci.c, nvme.c, pci.c, bios.c + biosthunk.asm (вызов BIOS из ядра),
                          biosdisk.c, win32.asm (вход в код Windows-программ), hal.c (x86_main + HAL)
include/synth/            заголовки: hal.h, console.h, theme.h, vfs.h, shell.h, command.h, exec.h,
                          winpath.h, vmm.h, fbcon.h, font.h, arch_win32.h …
kernel/main.c             kmain: порядок инициализации
kernel/core/              console, fbcon (графическая консоль), font8x16 (шрифт), theme, input, stream,
                          codepage, winpath (пути C:\…), block, time, log
kernel/lib/               libc.c (mem*/str*), strutil.c, fmt.c (printf), path.c, misc.c
kernel/mm/heap.c          аллокатор (корзины по размеру, кучи-экземпляры для процессов)
kernel/win32/             подсистема Windows: pe.c (загрузчик PE), process.c (процесс, TEB/PEB,
                          вложенный запуск), kernel32.c, files.c, console.c, locale.c, text.c (кодировки),
                          user32.c, system.c (advapi32, shell32, ntdll), stubs.c
kernel/win32/crt/         msvcrt: crt.c (старт, выход, окружение), stdio.c, format_fp.c (printf/scanf),
                          stdlib.c (строки, ctype, conio), math_fp.c (x87), time.c
kernel/fs/                vfs.c, ramfs.c, devfs.c, sfs.c (SynthFS), ramdisk.c
kernel/shell/             parse, run, script, shell, lineedit, interactive, prompt, settings, calc, command, state
kernel/cmds/              files.c, text.c, shell.c, system.c, custom.c — встроенные команды
kernel/apps/              edit.c, more.c
kernel/exec/              exec.c (поиск/запуск), script.c (формат .sh)
kernel/vm/                vm.c (интерпретатор SX), asm.c, opcodes.c, commands.c (asm/dis)
kernel/dos/               cpu.c (8086/186), dos.c (память/загрузчик/процессы), int21.c, bios.c
programs/sx/              программы SX (*.sasm) + synth.inc — попадают в /bin и /home/src
programs/dos/             DOS-программы (*.asm → /dos/*.com, exe/*.asm → /dos/*.exe)
programs/win32/           Windows-программы (*.c, *.cpp → /apps/*.exe, dll/*.c → /apps/*.dll),
                          prebuilt/ — готовые .exe для сборки без MinGW
rootfs/                   содержимое корневой ФС: /etc (synthrc, motd, themes), /docs, /home, /bin (*.sh)
tools/mkrd.c              упаковщик rootfs в ramdisk (формат SRD1)
tools/sasm.c              ассемблер SX для хоста (использует kernel/vm/asm.c)
tools/mkfont.py           генератор шрифта 8×16 (нужен только для изменения шрифта, требует Pillow)
tests/run.sh              автотесты через COM-порт; tests/hw.sh — матрица «железа»;
                          tests/shot.sh — скриншоты VGA через QEMU
```

---

## 6. Как расширять систему

### Новая команда

Создать файл в `kernel/cmds/` (или дописать в подходящий) — регистрация автоматическая:

```c
#include <synth/command.h>

COMMAND(hi, "hey,privet", "custom", "hi [name]", "say hello")
{
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "Hello, %s!\n", argc > 1 ? argv[1] : "world");
    sh_reset(sh);
    return 0;
}
```

Параметры: имя (идентификатор), алиасы через запятую или `NULL`, категория
(`files`, `text`, `shell`, `system`, `custom`, `dev`), строка использования, описание.
Вывод — только через `sh_printf`/`sh_puts` (тогда работают `>` и `|`), цвета — через роли
(`sh_role`), ошибки — `sh_error` / `sh_fs_error`. Флаги — `getopt_flags(sh, argc, argv, "la", &flags)`
и `flags & OPT('l')`. Долгие циклы проверяют `sh_interrupted()` (Ctrl+C).

### Новая настройка для `config`

```c
static void get(char *buf, size_t cap) { ... }
static int set(struct shell *sh, const char *value) { ... return 0 или E_INVAL; }
static const struct setting s_name = {"name", "on|off", "description", get, set};
SETTING(s_name);
```
`config save` сохранит её автоматически.

### Новая тема

* Встроенная — добавить элемент в `builtin_themes[]` в `kernel/core/theme.c`.
* Файловая — положить `rootfs/etc/themes/имя.theme` (пример: `sunset.theme`). Ключи:
  `name`, `description`, `base`, `color0`…`color15 = #rrggbb`, роли вида `accent = pink on black`.

### Новый формат программ

```c
static bool probe(const char *path, const uint8_t *head, size_t n) { ... }
static int run(struct exec_ctx *ctx, uint8_t *image, size_t size) { ... }
static const struct binfmt fmt = {"name", "description", ".ext", probe, run};
BINFMT(fmt);
```

### Новая файловая система

Реализовать `struct vnode_ops` и `struct fs_type { name, description, mount, statfs }`,
зарегистрировать `FS_TYPE(...)`. Пример попроще — `kernel/fs/ramfs.c`, дисковый — `kernel/fs/sfs.c`.
Монтирование: `mount <fs> <источник> <каталог>`.

### Новый драйвер диска

Заполнить `struct blockdev` (`read`, `write`, `sectors`, `name`) и вызвать `block_register()`
из `hal_fs_init()` своей платформы (см. `arch/x86/ata.c`, `ahci.c`, `nvme.c`). PCI-устройства
находятся через `pci_foreach()`, регистры отображаются `vmm_map_phys()`, физический адрес буфера
для DMA — `vmm_phys()` (память из `kmalloc_pages` физически непрерывна).

### Новая функция Windows API

Функции пишутся как обычные C-функции с соглашением `WINAPI` (stdcall) для kernel32/user32/…
или `cdecl` для msvcrt и регистрируются таблицей экспорта:

```c
#include "win32.h"

static BOOL WINAPI Beep(DWORD freq, DWORD ms) { hal_beep(freq, ms); return TRUE; }

static const struct w32_export my_exports[] = {W32_FN(Beep), W32_AS("MessageBeep", w32_one1)};
W32_EXPORTS(my_module, "kernel32.dll", my_exports);
```

Одна DLL может состоять из нескольких таблиц в разных файлах. Новая DLL — просто новое имя в
`W32_EXPORTS`; она сама появится в `C:\Windows\System32`. Заглушки с правильным числом аргументов —
`w32_zero0…6`, `w32_one0…6` (для stdcall это важно: функция сама снимает аргументы со стека).
Глобальное состояние модуля помечается `W32_STATE` (оно сохраняется при вложенном запуске), а
сброс на старте процесса — в `.attach` таблицы. Каких функций не хватает программе — видно сразу:
она остановится с сообщением `… is not supported yet`.

### Порт на новое железо (ARM, RISC-V, калькулятор…)

Создать `arch/<имя>/` и реализовать функции из `include/synth/hal.h`: дисплей (сеточный —
`present/cursor/palette`, либо потоковый — `write`), `hal_key_read`, время, `hal_reboot/poweroff`,
`hal_fs_init`; стартовый код должен заполнить `struct boot_info` (куча, ramdisk) и вызвать `kmain()`.
В Makefile добавить выбор платформы (сейчас жёстко `arch/x86`). Ядро, оболочка, VM SX и эмулятор
DOS переносятся без изменений (эмулятор DOS требует ~1 МБ ОЗУ, отключается `CONFIG_DOS=0`).

### Программы для SynthOS

* SX: положить `programs/sx/имя.sasm` — соберётся в `/bin/имя.sx`, исходник попадёт в `/home/src`.
  Справочник — `rootfs/docs/sx.txt`, примеры — `hello`, `guess`, `snake`.
* DOS: `programs/dos/имя.asm` (nasm, `org 0x100`) → `/dos/имя.com`;
  `programs/dos/exe/имя.asm` → `/dos/имя.exe`.
* Windows: `programs/win32/имя.c` или `.cpp` → `/apps/имя.exe` (собирается
  `i686-w64-mingw32-gcc`, C++ — со `-static`), `programs/win32/dll/имя.c` → `/apps/имя.dll`.
  Готовые `.exe` можно просто положить в `/home` — они запустятся по имени.
* Скрипты и любые файлы — в `rootfs/` (каталог копируется в корень ОС).

---

## 7. Правила кода

* C11 (gnu11), 4 пробела, строки до ~110 символов (`.clang-format` в корне).
* Комментариев почти нет: код должен читаться сам — говорящие имена, короткие функции.
* Ядро не использует стандартную библиотеку C: только `synth/string.h`, `synth/fmt.h`,
  `synth/heap.h` (`kmalloc/kfree`) и т. д.
* Ошибки — отрицательные коды `E_*` из `synth/error.h`, текст — `err_str()`.
* Имена — `модуль_действие` (`vfs_open`, `con_put_cell`, `theme_load`); статические помощники
  без префикса.
* Никаких цветов «в лоб» — только роли темы; никакого прямого вывода на экран из команд — только
  через `sh->out`.
* Всё, что зависит от железа, — только в `arch/`.
* Перед коммитом: `make && make test` без предупреждений и с 0 упавших тестов.

---

## 8. Тестирование

* `make test` — собирает образ с консолью на COM-порту, запускает QEMU без окна, вводит команды из
  `tests/run.sh` и проверяет вывод. Новый тест = две строки в списке: команда и ожидаемый фрагмент.
* `make test-hw` — тот же образ на 8 конфигурациях: BIOS и UEFI (нужен пакет `ovmf`), диски IDE,
  AHCI, NVMe, USB, процессор 486; проверяются оболочка, запуск Windows-программы и сохранение
  `/home` после перезагрузки (~3–4 минуты).
* `tests/shot.sh build/vga/synthos.img out.png 2 <клавиши QEMU…>` — запускает VGA-образ, нажимает
  клавиши (`l s ret`, `shift-apostrophe`, `up`…) и сохраняет скриншот (нужны `socat` и Python PIL).

---

## 9. Дорожная карта (идеи, по приоритету)

1. **USB:** контроллеры xHCI/EHCI, клавиатура USB HID и флешки (USB Mass Storage) — чтобы не
   зависеть от эмуляции PS/2 и сохранять `/home` на флешке при загрузке через UEFI.
2. **FAT12/16/32** — чтение/запись флешек и разделов Windows (`mount fat ...`), раздел UEFI.
3. **Windows дальше:** окна (GDI/user32 поверх фреймбуфера), потоки (`CreateThread`), сеть
   (ws2_32), UCRT (`api-ms-win-crt-*` с новыми именами функций), 64-битные программы
   (ядро x86-64 или режим совместимости).
4. **Многозадачность:** процессы, планировщик, фоновые задачи (`cmd &`), `ps`/`kill`.
5. **PAE** — память и устройства выше 4 ГБ.
6. **Язык высокого уровня** для SX (мини-C или свой скриптовый язык) с компилятором внутри ОС.
7. **Эмулятор DOS:** 386-инструкции, EMS/XMS, звук PC-speaker, мышь INT 33h, графика 13h.
8. **Порты:** ARM (Raspberry Pi), RISC-V, графические калькуляторы — через новый `arch/`.
9. **Сеть:** драйвер NE2000/virtio-net/e1000, стек TCP/IP, `ping`, `wget`.

---

## 10. Для Claude в новом чате

1. Прочитать этот файл, затем `docs/ARCHITECTURE.md` и `CLAUDE.md`.
2. Проверить окружение: `make && make test` — должно быть `tests: 63 passed, 0 failed`;
   при наличии `ovmf` — `make test-hw` (`hardware tests: 23 passed, 0 failed`).
3. Визуальная проверка: `tests/shot.sh build/vga/synthos.img /tmp/shot.png 2 f e t c h ret`.
4. Соблюдать правила из раздела 7, расширять через макросы из раздела 6, добавлять тесты в
   `tests/run.sh` для всего нового.
5. Известные ограничения: нет драйверов USB (флешки и USB-клавиатуры работают через BIOS/эмуляцию
   PS/2); ядро 32-битное без PAE (видит до ~3.75 ГБ ОЗУ, устройства выше 4 ГБ недоступны, кроме
   перемещаемых BAR на шине 0); Windows — только 32-битные консольные программы без потоков и окон;
   эмулятор DOS — только 8086/186 и текстовый режим; одна задача одновременно (но программа может
   запускать другие); пользователь один (root).
