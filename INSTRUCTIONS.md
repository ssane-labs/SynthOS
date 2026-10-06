# SynthOS — инструкция

Этот файл — главная точка входа в проект. Его достаточно прочитать (человеку или Claude в новом
чате), чтобы собрать систему, запустить её и продолжить разработку.

* Что умеет система и как выглядит — [README.md](README.md)
* Как она устроена внутри — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
* Справка внутри самой ОС — команды `help`, `more /home/readme.txt`, файлы в `/docs`

---

## 1. Текущее состояние

SynthOS 0.1.0 «Neon» — рабочая база:

* свой загрузчик (MBR + stage2), 32-битное ядро для x86 (BIOS), запуск в QEMU и на реальном ПК;
* VGA-консоль с темами, клавиатура PS/2, таймер, RTC, COM-порт, ATA-диск;
* файловые системы: ramfs (корень), devfs, своя дисковая **SynthFS** (`/home` сохраняется);
* оболочка с 82 командами, скриптами, пайпами, подсветкой синтаксиса и автодополнением;
* виртуальная машина **SX** для своих программ + ассемблер/дизассемблер внутри ОС;
* эмулятор **MS-DOS** (8086/186) для `.COM` и `.EXE`;
* редактор `edit`, пейджер `more`, игра `snake`;
* автотесты (`make test`, 45 проверок — все проходят).

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
   sudo apt install -y build-essential nasm qemu-system-x86 git
   ```
3. Скопировать репозиторий внутрь WSL (так сборка быстрее), например:
   `git clone <адрес репозитория> ~/SynthOS` или `cp -r /mnt/c/путь/SynthOS ~/SynthOS`.
4. QEMU из WSL открывает окно через WSLg (Windows 11 — из коробки). Если окна нет — установите
   QEMU для Windows (qemu.org) и запускайте образ оттуда:
   `qemu-system-i386 -drive format=raw,file=synthos.img -m 64`.

### Linux (Ubuntu / Debian / Mint)

```sh
sudo apt install -y build-essential nasm qemu-system-x86 git
```
(Fedora: `sudo dnf install gcc make nasm qemu-system-x86`; Arch: `sudo pacman -S base-devel nasm qemu-full`.)

### macOS

```sh
brew install i686-elf-gcc i686-elf-binutils nasm qemu
```
Makefile сам найдёт кросс-компилятор `i686-elf-gcc`.

---

## 3. Сборка и запуск

```sh
cd SynthOS
make              # собрать build/vga/synthos.img (загрузочный образ диска 16 МБ)
make run          # запустить в QEMU (окно с VGA-экраном)
make run-serial   # запустить с консолью в текущем терминале через COM-порт
make test         # собрать вариант с COM-консолью и прогнать автотесты
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
2. В BIOS/UEFI включить **Legacy/CSM-загрузку** и загрузиться с флешки.

Ограничения на железе: нужна загрузка через BIOS (не чистый UEFI); сохранение `/home` работает
только для дисков IDE/ATA (или SATA в режиме IDE) — драйверов USB и AHCI пока нет, поэтому при
загрузке с флешки `/home` живёт в памяти. Чужие диски система не трогает (проверяет сигнатуру
своего образа перед использованием раздела).

---

## 4. Как пользоваться (коротко)

```text
help / help ls            список команд / справка по команде
ls -l, cd, tree, find     файлы; edit файл — редактор; more файл — просмотр
theme, prompt, color      оформление; config — все настройки; config save — сохранить
hello, fib 20, snake      программы SX из /bin
hello.com, box.com        программы DOS из /dos
asm файл.sasm             собрать свою программу; dis файл.sx — дизассемблировать
```

Скрипты: файл `.sh` с командами оболочки (`if … end`, `while … end`, `for x in … end`,
`func имя … end`). Стартовый скрипт пользователя — `/home/.synthrc`.
Полный синтаксис — `more /docs/shell.txt` внутри ОС.

---

## 5. Структура репозитория

```text
Makefile, config.mk       сборка и параметры (размер образа, включаемые подсистемы)
arch/x86/boot/            stage1.asm (MBR), stage2.asm, layout.inc (карта диска и памяти)
arch/x86/                 entry.asm, isr.asm, linker.ld, cpu.c (GDT/IDT/PIC), timer.c (PIT/RTC/звук),
                          keyboard.c, serial.c, vga.c, ata.c, hal.c (x86_main + HAL), libgcc.c
include/synth/            заголовки: hal.h, console.h, theme.h, vfs.h, shell.h, command.h, exec.h, sx.h …
kernel/main.c             kmain: порядок инициализации
kernel/core/              console, theme, input, stream, codepage, block, time, log
kernel/lib/               libc.c (mem*/str*), strutil.c, fmt.c (printf), path.c, misc.c
kernel/mm/heap.c          аллокатор (best-fit со слиянием блоков)
kernel/fs/                vfs.c, ramfs.c, devfs.c, sfs.c (SynthFS), ramdisk.c
kernel/shell/             parse, run, script, shell, lineedit, interactive, prompt, settings, calc, command, state
kernel/cmds/              files.c, text.c, shell.c, system.c, custom.c — встроенные команды
kernel/apps/              edit.c, more.c
kernel/exec/              exec.c (поиск/запуск), script.c (формат .sh)
kernel/vm/                vm.c (интерпретатор SX), asm.c, opcodes.c, commands.c (asm/dis)
kernel/dos/               cpu.c (8086/186), dos.c (память/загрузчик/процессы), int21.c, bios.c
programs/sx/              программы SX (*.sasm) + synth.inc — попадают в /bin и /home/src
programs/dos/             DOS-программы (*.asm → /dos/*.com, exe/*.asm → /dos/*.exe)
rootfs/                   содержимое корневой ФС: /etc (synthrc, motd, themes), /docs, /home, /bin (*.sh)
tools/mkrd.c              упаковщик rootfs в ramdisk (формат SRD1)
tools/sasm.c              ассемблер SX для хоста (использует kernel/vm/asm.c)
tests/run.sh              автотесты через COM-порт; tests/shot.sh — скриншоты VGA через QEMU
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
из `hal_fs_init()` своей платформы (см. `arch/x86/ata.c`).

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
* `tests/shot.sh build/vga/synthos.img out.png 2 <клавиши QEMU…>` — запускает VGA-образ, нажимает
  клавиши (`l s ret`, `shift-apostrophe`, `up`…) и сохраняет скриншот (нужны `socat` и Python PIL).

---

## 9. Дорожная карта (идеи, по приоритету)

1. **Драйверы хранения:** AHCI (SATA) и USB Mass Storage — чтобы `/home` сохранялся на флешке и
   современных ПК; поддержка FAT12/16/32 для обмена файлами с Windows.
2. **Графика:** VESA/VBE фреймбуфер, шрифт с кириллицей (CP866), графический режим 13h для DOS-игр.
3. **Многозадачность:** процессы, планировщик, фоновые задачи (`cmd &`), `ps`/`kill`.
4. **Язык высокого уровня** для SX (мини-C или свой скриптовый язык) с компилятором внутри ОС.
5. **Эмулятор DOS:** 386-инструкции, EMS/XMS, звук PC-speaker, мышь INT 33h.
6. **UEFI-загрузчик** для машин без CSM.
7. **Порты:** ARM (Raspberry Pi), RISC-V, графические калькуляторы — через новый `arch/`.
8. **Сеть:** драйвер NE2000/virtio-net, стек TCP/IP, `ping`, `wget`.

---

## 10. Для Claude в новом чате

1. Прочитать этот файл, затем `docs/ARCHITECTURE.md` и `CLAUDE.md`.
2. Проверить окружение: `make && make test` — должно быть `tests: 45 passed, 0 failed`.
3. Визуальная проверка: `tests/shot.sh build/vga/synthos.img /tmp/shot.png 2 f e t c h ret`.
4. Соблюдать правила из раздела 7, расширять через макросы из раздела 6, добавлять тесты в
   `tests/run.sh` для всего нового.
5. Известные ограничения: только x86 BIOS; нет USB/AHCI; нет графики; эмулятор DOS — только
   8086/186 и текстовый режим; одна задача одновременно; пользователь один (root).
