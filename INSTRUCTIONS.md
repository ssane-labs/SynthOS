# SynthOS — инструкция

Этот файл — главная точка входа в проект. Его достаточно прочитать (человеку или Claude в новом
чате), чтобы собрать систему, запустить её и продолжить разработку.

* Что умеет система и как выглядит — [README.md](README.md)
* Как она устроена внутри — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
* Справка внутри самой ОС — команды `help`, `more /home/readme.txt`, файлы в `/docs`

---

## 1. Текущее состояние

SynthOS 1.0 «Mirage» — рабочая система:

* свой загрузчик для **BIOS** (MBR + stage2, LBA или CHS, карта памяти E820/E801/88h) и свой
  загрузчик для **UEFI** (`BOOTX64.EFI`) — один и тот же образ грузится на старых и новых ПК;
* 32-битное ядро x86 (от **i486**), страничная память, **PAE** (ОЗУ и устройства выше 4 ГБ),
  переход в **long mode** на время работы 64-битных программ; сэмплирующий профилировщик `prof`;
* **рабочий стол с тремя образами** (`config look dune|mist|night`, Настройки → Looks): у
  каждого свои обои — «бумажные» пейзажи из слоёв с мягкими тенями (дюны с солнцем, туманные горы,
  ночные холмы под луной и звёздами; рисуются кодом в разрешении экрана, светлый и тёмный вариант),
  палитра и раскладка оболочки:
  * **Dune** — тёплое стекло, угловая панель запуска внизу слева (щелчок по «Apps» раскрывает сетку
    всех приложений), капсула статуса справа;
  * **Mist** — строка меню сверху, плавающий Dock по центру;
  * **Night** — лента приложений слева, тёмная палитра;
  * виджетов по умолчанию нет ни в одном образе — стол чистый, их добавляют сами (см. ниже);
  * **в любом образе можно настроить всё** (Настройки → Desktop & Widgets, `config dockstyle
    bottom|corner|side`, `config statusbar menubar|pill`): Dock внизу, в углу или
    сбоку, строка меню или капсула статуса; у каждого образа своя раскладка
    (`~/.looks`), кнопка Reset возвращает исходную;
* **Dock — ваш**: значок перетаскивается по Dock (под курсором открывается промежуток, остальные
  плавно расступаются), вытащенный с Dock исчезает облачком (приложение, брошенное на стол, ложится
  ярлыком `Имя.app`), любой значок со стола перетаскивается на Dock в нужное место; в Dock можно
  держать и файлы; у каждого образа свой набор (`~/.dock`, `config dockapps`); в меню значка —
  «Keep in Dock / Remove from Dock»; по умолчанию в Dock только самое нужное: приложения, Файлы, Chromium,
  Терминал и Корзина;
* **виджеты** на стекле: часы, погода (с wttr.in, DNS и сокет без ожидания; город — `config weathercity`),
  музыка, календарь, задачи (`~/.tasks`), визуализатор спектра; по умолчанию их нет — добавляются
  в режиме редактирования или `widgets add clock`; перетаскиваются; **режим редактирования** (правый клик по столу или виджету → Edit Widgets,
  `widgets edit`): окна расходятся, у виджетов значки «−», внизу галерея добавляет любой из шести;
  добавленные выплывают, удалённые тонут, остальные переезжают на новые места; у каждого образа свой
  набор (`~/.widgets`, `widgets add|remove`, `config widgets`);
* **Keep on Top**: в меню «⋯» любого окна — «Keep on Top»: окно остаётся поверх остальных, даже когда
  фокус у другого (булавка в заголовке; `windows pin N`, у Windows-программ — `HWND_TOPMOST`);
* **обои из интернета**: Настройки → Wallpaper → From the Internet — фото дня Bing, случайное фото
  (picsum.photos) или любая ссылка на картинку; `wallpaper bing|random|<url>`; файлы ложатся в
  `~/Pictures/Wallpapers`; светлое стекло само осветляется над тёмной картинкой;
* анимации спокойные и только на действия: никакой пружинистости (кривые с критическим затуханием —
  без перелёта и покачиваний), ничего не подпрыгивает и не растёт под курсором; при нажатии значок
  в Dock чуть «утапливается», запускаемое приложение один раз мягко подскакивает;
  положение считается с дробной частью, поэтому движение доходит до места без рывка в конце; окно
  открывается из 92 % с лёгким подъёмом и проявлением, закрывается ускоряясь, при движении картинка
  окна масштабируется сглаженно (билинейно) и рисуется в своём слое, так что последний кадр совпадает
  с окном; «джинн» при сворачивании, плавная смена образа; цветных свечений нет;
  `config animations off` — выключить;
* значки плоские: ровный цвет без засветки сверху, затемнения снизу и теней;
* **минимализм**: у каждого окна — только название и одна кнопка «⋯», в выпадающем списке все команды
  приложения (и «Свернуть/Во весь экран/Закрыть»); Блокнот показывает только текст, Файлы — только
  содержимое папки, кнопки «назад/вперёд/вверх» и путь (поиск — просто начать печатать), у Фото
  внизу капсула «‹ − + ⟳ ›», у Видео — пауза, перемотка и время (появляются при движении мыши), в Paint все инструменты — в маленькой капсуле
  внизу, которая раскрывается под курсором, у Системного монитора и Сапёра вкладки и уровни — в «⋯»;
* **браузер — Chromium, предустановлен**: образ несёт сборку Chromium (открытый браузер, на котором
  построен Chrome) отдельной областью диска между системой и разделом данных (`tools/mkbundle.py`,
  `make` скачивает снимок сборки сам). Через несколько секунд после первого входа он без всякой сети
  распаковывается в `/home/apps/chromium` (полоска под значком в Dock; около 40 секунд даже в эмуляторе),
  и дальше открывается сразу — значок Chromium в Dock, ссылки и `.html` открываются в нём; то же вручную —
  `bundle` и `bundle extract папка`. Если образ собран без него, щелчок по значку ставит Google Chrome
  из сети (ниже);
* **Google Chrome** — по желанию (`launch Chrome`, `pkg install chrome`); образ без Chromium через
  15 секунд после первого входа (если есть сеть и место на диске) сам начинает качать Chrome в фоне — полоска под значком в Dock показывает ход,
  а когда всё готово, приходит уведомление (`config chromeauto off` — не качать самому). Можно и не ждать:
  щелчок по значку показывает то же окно с ходом загрузки. SynthOS спрашивает сервер
  обновлений Google, скачивает официальный офлайн-пакет сжатым gzip (231 МБ вместо 443 МБ, распаковка
  на лету, окно TCP до 1 МБ и подтверждения через сегмент, SHA-256 проверяется, свободное место — заранее) и распаковывает программу прямо из 7z-архива внутри установщика в `/home/apps/chrome`
  (свой распаковщик 7z: LZMA, BCJ2, команда `un7z`), кладёт ярлык на стол и открывает Chrome; то же —
  `pkg install chrome`. Нужно ~900 МБ свободного места: диск ВМ должен быть хотя бы 1,5 ГБ;
* приложения — Chromium, Терминал, Файлы, Блокнот, **Фото** (PNG, JPEG baseline/progressive, GIF с
  анимацией, BMP; масштаб, поворот, слайд-шоу, обои из фото), **Музыка** (встроенный синтезатор с тремя
  своими композициями и WAV-файлы), **Видео** (AVI с Motion JPEG и PCM-звуком), **Просмотр** (любой
  файл текстом или в hex), Paint, Калькулятор, Системный монитор, Сапёр, Системные настройки,
  **Установка SynthOS**; **автозапуск** (Настройки → Login Items, команда `autostart`);
  композитный оконный менеджер, «жидкое стекло» любой формы, вход по паролю, блокировка экрана,
  несколько мониторов;
* **звук**: драйвер Intel AC'97 (16 бит, стерео, кольцо DMA-буферов); в QEMU — `-device AC97`
  (`make run` добавляет его сам), команда `music`;
* **установщик**: `install` (или приложение «Install SynthOS») копирует систему на другой диск,
  растягивает раздел данных на весь диск и по желанию переносит `/home`; новый диск грузится и в
  BIOS, и в UEFI;
* консоль: VGA-текст или графический фреймбуфер, **свой шрифт 8×16 с кириллицей** (CP437/CP866), темы;
* устройства: клавиатура и мышь PS/2, **USB** (xHCI, хабы, клавиатуры, мыши, флешки), диски
  IDE/ATA (**bus-master DMA**), **AHCI**, **NVMe**, на BIOS-машинах — любой диск через BIOS;
  сетевые карты **e1000/e1000e, RTL8139, RTL8169**;
* файловые системы: ramfs (корень), devfs, своя дисковая **SynthFS** (`/home` сохраняется, файлы до ~1 ГБ),
  **FAT12/16/32** с длинными именами (флешки и разделы монтируются сами в `/mnt/…`);
  структура каталогов в стиле Windows: `C:\Windows`, `C:\Program Files`, `C:\Users\<имя>`;
  `C:\Program Files`, `C:\ProgramData` и `C:\Windows\Temp` лежат на постоянном диске
  (`/home/apps`, `/home/programdata`, `/home/tmp`), так что установленные программы не пропадают;
* **отзывчивость**: указатель мыши рисуется прямо из прерывания мыши поверх экрана, поэтому он движется
  плавно, даже когда система занята (тяжёлая команда, загрузка, программа); планировщик отдаёт рабочему
  столу процессор не реже чем раз в 30 мс, а при нажатой клавише или движении мыши — в течение 2 мс,
  так что занятые программы Windows не «замораживают» экран;
* **сеть**: свой TCP/IP (масштаб окна до 1 МБ, очередь сегментов не по порядку), DHCP, DNS, сокеты; **TLS 1.3 и 1.2** (AES-GCM, ChaCha20-Poly1305, X25519,
  P-256/P-384, RSA/ECDSA) с проверкой цепочки сертификатов по корням Mozilla; HTTP/HTTPS-клиент
  с редиректами и cookies; команды `wget`, `browse`, `ping`, `nslookup`, `httpd`, `certs`;
* HTML-движок (текст, ссылки, списки, таблицы, формы GET/POST, кодировки) выводит страницы текстом в
  терминале (`browse`); для окон — Chromium или Google Chrome (см. выше);
* оболочка со 116 командами, скриптами, пайпами, подсветкой синтаксиса и автодополнением;
* **запуск программ Windows**: 32-битные (PE32) и **64-битные (PE32+)** `.exe`/`.dll`, консольные и
  **оконные** (окна, GDI, элементы управления, диалоги из ресурсов), потоки и синхронизация,
  Winsock, **WinINet/WinHTTP/URLDownloadToFile** с HTTPS, msvcrt и **UCRT**, C++ с исключениями
  (в 64-битных — табличная раскрутка x64 SEH); многопроцессный **Google Chrome** (32-битный) открывается
  окном на рабочем столе (вкладки, адресная строка, страницы `file://` и `http://` с JavaScript),
  а в режиме `--headless` печатает DOM (`--dump-dom`) и сохраняет снимок страницы (`--screenshot`);
  запускается как обычно — двойным щелчком по `chrome.exe` или ярлыку: нужные ключи
  (`--no-sandbox --test-type --in-process-gpu --disable-gpu --disable-software-rasterizer`) система
  добавляет сама; свои кнопки «свернуть/развернуть/закрыть» Chrome скрыты, остаются кнопки окна ОС;
  работают клавиатура (Ctrl+L, ввод адреса) и мышь (ссылки); HTTPS проверяется встроенными корнями Chrome
  плюс хранилищем Windows `ROOT` (корни Mozilla и сертификаты из `/home/certs` после `certs reload`); шрифты для DirectWrite — встроенный
  `C:\Windows\Fonts\DejaVuSans.ttf` (латиница и кириллица, `tools/mksysfont.sh`) и любые TrueType-файлы,
  скопированные в `/home/fonts`;
* пользователи с паролями (`useradd`, `passwd`, `su`, `lock`), у каждого свой `/home`;
* виртуальная машина **SX** для своих программ + ассемблер/дизассемблер внутри ОС;
* эмулятор **MS-DOS** (8086/186) для `.COM` и `.EXE`;
* автотесты: `make test` (173 проверки), `make test-hw` (BIOS/UEFI × IDE/AHCI/NVMe/USB/xHCI, i486;
  29 проверок), `make test-crypto` (криптография против эталонов).

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
   `qemu-system-x86_64 -drive format=raw,file=synthos.img -m 256`.

### Linux (Ubuntu / Debian / Mint)

```sh
sudo apt install -y build-essential nasm qemu-system-x86 git \
    gcc-mingw-w64-i686 g++-mingw-w64-i686 gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
    dosfstools mtools ovmf openssl python3-pil socat
```
(`openssl` нужен тестам HTTPS, `python3-pil` и `socat` — скриншотам; Fedora: `sudo dnf install gcc make nasm qemu-system-x86 mingw32-gcc mingw32-gcc-c++ mingw64-gcc dosfstools mtools edk2-ovmf`;
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
make              # собрать build/vga/synthos.img (загрузочный образ диска 32 МБ, BIOS + UEFI)
make run          # запустить в QEMU как ПК с BIOS (окно с экраном, 256 МБ, сетевая карта e1000)
make run-uefi     # запустить в QEMU как современный ПК с UEFI (графическая консоль)
make run-serial   # запустить с консолью в текущем терминале через COM-порт
make test         # собрать вариант с COM-консолью и прогнать автотесты
make test-hw      # прогнать образ на 10 разных «машинах» (BIOS/UEFI, IDE/AHCI/NVMe/USB/xHCI, i486)
make test-crypto  # проверить SHA-2, AES-GCM, ChaCha20, X25519, RSA, ECDSA на хосте
make clean        # удалить build/
```

Полезное:

* Выход из QEMU: команда `shutdown` внутри SynthOS (или закрыть окно; в `run-serial` — `Ctrl+A`, затем `X`).
* Файлы в `/home` хранятся в самом образе. Чтобы начать «с чистого диска» — `make clean && make`.
* Отключение подсистем для маленьких машин (`config.mk` или командная строка):
  `make CONFIG_DOS=0 CONFIG_SXVM=0 CONFIG_EDITOR=0`.
* Дополнительные флаги QEMU: `make run QEMU_FLAGS="-smp 2 -device qemu-xhci -device usb-kbd"`.
* Сеть в QEMU работает сразу (user-mode, адрес по DHCP). Сертификаты HTTPS проверяются по корням
  из `/etc/certs` (генерируются `tools/mkcerts.sh`); свои корни — файлы `.pem` в `/home/certs`
  и команда `certs reload`.

### На реальном компьютере

1. Записать образ на флешку **побайтно** (все данные на флешке пропадут):
   * Linux/WSL: `sudo dd if=build/vga/synthos.img of=/dev/sdX bs=1M status=progress && sync`
   * Windows: Rufus → выбрать `synthos.img` → режим **DD-образ**.
2. Загрузиться с флешки через меню загрузки (обычно F12, F11, F8 или Esc). Подходит и
   **UEFI**, и **Legacy/CSM** — образ гибридный. **Secure Boot** нужно выключить
   (загрузчик не подписан).

Где сохраняется `/home`: на диске, с которого загрузилась система, если это IDE, SATA (AHCI)
или NVMe; при загрузке через BIOS — на любом диске, который видит BIOS (включая USB-флешку).
При загрузке через UEFI с USB-флешки `/home` хранится на ней же через свой драйвер xHCI.
Чужие диски система не трогает: раздел используется, только если на диске лежит именно
загрузочный образ SynthOS (сигнатура в MBR + заголовок ядра).

Требования к железу: процессор x86 от i486 и 6 МБ ОЗУ (программам Windows — от 8 МБ; программы,
собранные MinGW «под i686», используют инструкции CMOV и требуют Pentium Pro и новее; 64-битным
программам нужен процессор x86-64), клавиатура PS/2 или USB. Видео: VGA (BIOS) или любой
фреймбуфер GOP (UEFI). Сеть: Intel e1000/e1000e или Realtek RTL8139/RTL8169.

---

## 4. Как пользоваться (коротко)

После загрузки сразу открывается **рабочий стол**. Терминал на нём — обычная оболочка SynthOS.

```text
Мышь                      окна: перетаскивать за заголовок, края — менять размер,
                          двойной щелчок по заголовку — развернуть; правая кнопка — контекстное меню
Win или Ctrl+Esc          Launchpad (все приложения);  Ctrl+Space — поиск
Alt+Tab, Alt+F4           следующее окно, закрыть окно; все команды приложения — кнопка «⋯» в заголовке
config look night         образ рабочего стола: dune, mist, night (или Настройки → Looks)
music, install, autostart музыка, установка на другой диск, программы при входе
launch                    список приложений;  launch paint, launch files /home, launch файл.bmp
двойной щелчок по .exe    оконная программа Windows (или ярлык .lnk) запускается без терминала,
                          со своим значком на рабочем столе и в Dock; консольные — в терминале
desktop off / exit        не запускать стол при загрузке (config save) / выйти в текстовый режим
desktop (или startx)      снова запустить рабочий стол из текстового режима
lock, logout              заблокировать экран, сменить пользователя
```

Команды оболочки:

```text
help / help ls            список команд / справка по команде
ls -l, cd, tree, find     файлы; edit файл — редактор; more файл — просмотр
cd "C:\Program Files"     пути Windows понимаются напрямую; drives, winpath — карта дисков
theme, prompt, color      оформление; config — все настройки; config save — сохранить
hello, fib 20, snake      программы SX из /bin
hello.com, box.com        программы DOS из /dos
winhello, wincpp, hello64 встроенные программы Windows из /apps
windialog, wingdi         оконные программы Windows
shortcut программа [имя]  ярлык Windows (.lnk) на рабочем столе; shortcut -l файл.lnk — куда ведёт
bundle [extract папка]    предустановленный пакет (Chromium): что внутри / распаковать
pkg install chrome        скачать официальный установщик Google Chrome в ~/Downloads и запустить его
asm файл.sasm             собрать свою программу; dis файл.sx — дизассемблировать
```

Интернет:

```text
launch browser [адрес]    браузер: строка адреса (или слова для поиска), ссылки мышью или Tab/Enter,
                          формы, Backspace — назад, Ctrl+L — к адресу, загрузки — в ~/Downloads
browse https://…          страница текстом в терминале, ссылки пронумерованы
wget [-O файл] https://…  скачать файл;  -k — не проверять сертификат
ifconfig, dhcp, ping, nslookup, netstat     сеть;  httpd [порт] [папка] — свой веб-сервер
certs [list|reload]       доверенные корневые сертификаты;  cookies [clear] — cookies сайтов
```

Скорость и отладка: `time команда`, `prof [-o файл] команда` (где процессор проводит время;
адреса переводятся в функции `tools/profsym.py build/vga/kernel.elf < файл`), `winbench [папка]`
(файлы, консоль, куча, вызовы API), `bench`.

Про Windows-программы подробно — `more /docs/windows.txt` внутри ОС, про интернет —
`more /docs/internet.txt`, про рабочий стол — `more /docs/desktop.txt`.

Скрипты: файл `.sh` с командами оболочки (`if … end`, `while … end`, `for x in … end`,
`func имя … end`). Стартовый скрипт пользователя — `/home/.synthrc`.
Полный синтаксис — `more /docs/shell.txt` внутри ОС.

---

## 5. Структура репозитория

```text
Makefile, config.mk       сборка и параметры (размер образа, включаемые подсистемы)
arch/x86/boot/            stage1.asm (MBR, LBA/CHS), stage2.asm (A20, E820/E801, ядро), layout.inc
arch/x86/efi/             UEFI-загрузчик: loader.c (BOOTX64.EFI), trampoline.asm (выход из long mode)
arch/x86/                 entry.asm, isr.asm, linker.ld, cpu.c (GDT/IDT/PIC, CPUID, long mode),
                          long.asm (переходы 32↔64 бит, шлюзы вызовов Win64, x64 SEH), timer.c (PIT и
                          профилировщик), paging.c (страницы, PAE), keyboard.c, mouse.c, serial.c,
                          vga.c, vbe.c, display.c, ata.c (PIO и DMA), ahci.c, nvme.c, pci.c,
                          xhci.c, usb.c, usbhub.c, usbhid.c, usbstor.c, e1000.c, realtek.c, audio.c (AC'97),
                          bios.c + biosthunk.asm, biosdisk.c, win32.asm, hal.c (x86_main + HAL)
include/synth/            заголовки: hal.h, console.h, theme.h, vfs.h, shell.h, command.h, exec.h,
                          net.h, tls.h, crypto.h, http.h, html.h, gui.h, users.h, vmm.h …
kernel/main.c             kmain: порядок инициализации
kernel/core/              console, fbcon, font8x16, theme, input, stream, codepage, winpath (пути C:\…),
                          block, users, time, log
kernel/lib/               libc.c (mem*/str*), strutil.c, fmt.c (printf), path.c, lz4.c, sha256.c, misc.c
kernel/mm/heap.c          аллокатор (корзины по размеру, кучи-экземпляры для процессов)
kernel/fs/                vfs.c, ramfs.c, devfs.c, sfs.c (SynthFS), fat.c (FAT12/16/32), automount.c, ramdisk.c
kernel/net/               core.c (Ethernet, ARP, IPv4, ICMP, UDP), tcp.c, socket.c, dhcp.c, dns.c,
                          tls.c (TLS 1.3/1.2), x509.c (сертификаты, корни), http.c (клиент, cookies),
                          html.c (разбор HTML для браузера и browse)
kernel/crypto/            hash.c (SHA-2, HMAC, HKDF), aes.c (AES, GCM), chacha.c (ChaCha20-Poly1305),
                          x25519.c, pk.c (RSA, P-256/P-384, ECDSA), random.c (DRBG)
kernel/win32/             pe.c (PE32/PE32+), process.c, thread.c, kernel32.c, files.c, console.c, locale.c,
                          text.c, user32.c, window.c, gdi.c, controls.c, dialog.c, resource.c,
                          winsock.c, inet.c (wininet, winhttp, urlmon), system.c, stubs.c;
                          win64*.c — 64-битные программы (таблицы, GUI, сокеты, SEH, математика)
kernel/win32/crt/         msvcrt: crt.c, stdio.c, format_fp.c (printf/scanf), stdlib.c, math_fp.c,
                          time.c; ucrt.c — функции UCRT (__stdio_common_*, api-ms-win-crt-*)
kernel/shell/             parse, run, script, shell, lineedit, interactive, prompt, settings, calc, command, state
kernel/cmds/              files.c, text.c, shell.c, system.c, custom.c, users.c, paths.c, net.c,
                          browse.c — встроенные команды
kernel/apps/              edit.c, more.c
kernel/gui/               gfx.c (рисование, сглаживание, свечение, фигуры), wm.c (окна, «⋯», анимации,
                          кроссфейд), look.c (образы Dune/Mist/Night), anim.c (пружины),
                          glass.c (стекло любой формы), desktop.c (цикл событий, значки, палитра),
                          menubar.c, dock.c (центр, угол, лента), panel.c (статус), widgets.c (виджеты, погода), media.c (синтезатор,
                          WAV, команда music), autostart.c, spotlight.c (поиск, Launchpad), lock.c,
                          wallpaper.c (бумажные обои из слоёв), apps/chromium.c (предустановленный браузер), ui.c, terminal.c, image.c; uifont.c и icons.c —
                          сгенерированы
kernel/gui/apps/          browser, files, notepad, photos, music, video, viewer, installer, paint,
                          calc_fp, settings, monitor, mines
kernel/lib/               png.c, jpeg.c, gif.c, picture.c (BMP и общий вход), inflate.c, zip.c, lz4.c, …
kernel/core/install.c     установка системы на другой диск (команда install)
kernel/exec/              exec.c (поиск/запуск), script.c (формат .sh)
kernel/vm/                vm.c (интерпретатор SX), asm.c, opcodes.c, commands.c (asm/dis)
kernel/dos/               cpu.c (8086/186), dos.c (память/загрузчик/процессы), int21.c, bios.c
programs/sx/              программы SX (*.sasm) + synth.inc — попадают в /bin и /home/src
programs/dos/             DOS-программы (*.asm → /dos/*.com, exe/*.asm → /dos/*.exe)
programs/win32/           32-битные Windows-программы (*.c, *.cpp, *.rc → /apps/*.exe, dll/*.c → /apps/*.dll)
programs/win64/           64-битные (hello64, cpp64, winnet64; hello64u — сборка с UCRT)
                          prebuilt/ — готовые .exe для сборки без MinGW
rootfs/                   корневая ФС: /etc (synthrc, motd, themes, certs/mozilla.pem), /docs, /home, /bin
tools/                    mkrd.c (ramdisk), sasm.c (ассемблер SX), mkfont.py, mkuifont.py, mkicons.py,
                          lz4pack.py, mkcerts.sh (корни Mozilla), profsym.py (адреса prof → функции)
tests/                    run.sh (автотесты), hw.sh (матрица железа), crypto/ (векторы криптографии),
                          shot.sh (скриншоты VGA через QEMU)
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

### Новый сетевой драйвер

Заполнить `struct netdev` (`send`, `receive`, `link`, MAC-адрес) и вызвать `net_register()` из
`hal_net_init()` (см. `arch/x86/e1000.c`, `realtek.c`). Стек сам получит адрес по DHCP; приём —
опросом из `net_poll()`, который вызывается в цикле ожидания ядра. Программы ядра работают через
`net_socket`, `net_connect`, `net_send`/`net_recv` (TCP) и `net_sendto`/`net_recvfrom` (UDP) из `synth/net.h`, HTTPS — через `http_fetch()` из `synth/http.h`.

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

64-битные программы (`kernel/win32/win64*.c`) вызывают те же 32-битные реализации: по умолчанию
аргументы обрезаются до 32 бит, а функции с указателями на структуры, `va_list`, `FILE*`, 64-битными
значениями или плавающей точкой описываются флагами в таблице `W64_EXPORTS`:
`W64_FLAG("fprintf", W64F_FMT | W64F_FILE)`, `W64_MATH("sqrt", M_D_D)`, своя обёртка —
`W64_FN("WSAStartup", h_wsa_startup)` с сигнатурой `uint64_t h(uint64_t *a, const uint64_t *f, void *t)`
(`a` — целые аргументы, `f` — регистры xmm, `t` — 32-битная функция, вызываемая через `T32(...)`).
Для обратных вызовов в 64-битный код — `arch_w64_call(fn, argc, args)`.

### Новое приложение рабочего стола

Файл в `kernel/gui/apps/`: окно открывается `wm_open`, содержимое рисуется и обрабатывает события
одной функцией-кадром через `ui_run` (immediate-mode: виджет и рисуется, и возвращает «нажат»),
приложение регистрируется макросом `GUI_APP` — и само появляется в меню «Пуск» и в `launch`:

```c
#include "../internal.h"

struct hello { struct ui_state st; int clicks; };

static void frame(struct ui *ui, void *data)
{
    struct hello *h = data;
    ui_panel(ui, (struct rect){0, 0, ui->win->surface.w, ui->win->surface.h}, style.window);
    if (ui_button(ui, (struct rect){20, 20, 120, 34}, "Click me"))
        h->clicks++;
}

static void paint(struct window *w, struct canvas *c) { ui_run(w, w->data, c, NULL, frame, w->data); }
static void event(struct window *w, struct gui_event *e) { ui_run(w, w->data, NULL, e, frame, w->data); }
static void destroy(struct window *w) { kfree(w->data); }
static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct hello *h = kzalloc(sizeof(*h));
    if (h && !wm_open("Hello", ICON_APP, 300, 200, &ops, h, WIN_RESIZABLE))
        kfree(h);
}

static const struct gui_app hello_app = {"Hello", "Example app", ICON_APP, launch, NULL};
GUI_APP(hello_app);
```

`struct ui_state` должна быть первым полем данных (или передавайте её отдельно). Цвета — только из
`style` (они берутся из текущей темы), шрифты — `font_sans`, `font_bold`, `font_title`, `font_mono`.
Поле `extensions` (`".txt;.md"`) связывает приложение с типами файлов в Проводнике. Периодическая
работа — `wm_set_timer(w, мс)` и событие `EV_TIMER`. Диалоги: `gui_message`, `ui_confirm`,
`ui_prompt`, контекстное меню — `ui_menu`. Новые значки — в `tools/mkicons.py`, новые символы
шрифта — в `SYMBOLS` в `tools/mkuifont.py` (затем перегенерировать `.c`).

### Порт на новое железо (ARM, RISC-V, калькулятор…)

Создать `arch/<имя>/` и реализовать функции из `include/synth/hal.h`: дисплей (сеточный —
`present/cursor/palette`, либо потоковый — `write`), `hal_key_read`, время, `hal_reboot/poweroff`,
`hal_fs_init`; для рабочего стола — `hal_graphics_enter/leave` (линейный фреймбуфер 16/24/32 бита)
и `hal_pointer_read`; стартовый код должен заполнить `struct boot_info` (куча, ramdisk) и вызвать `kmain()`.
В Makefile добавить выбор платформы (сейчас жёстко `arch/x86`). Ядро, оболочка, VM SX и эмулятор
DOS переносятся без изменений (эмулятор DOS требует ~1 МБ ОЗУ, отключается `CONFIG_DOS=0`).

### Программы для SynthOS

* SX: положить `programs/sx/имя.sasm` — соберётся в `/bin/имя.sx`, исходник попадёт в `/home/src`.
  Справочник — `rootfs/docs/sx.txt`, примеры — `hello`, `guess`, `snake`.
* DOS: `programs/dos/имя.asm` (nasm, `org 0x100`) → `/dos/имя.com`;
  `programs/dos/exe/имя.asm` → `/dos/имя.exe`.
* Windows: `programs/win32/имя.c` или `.cpp` (+ `имя.rc` с ресурсами) → `/apps/имя.exe`
  (собирается `i686-w64-mingw32-gcc`, C++ — со `-static`), `programs/win32/dll/имя.c` →
  `/apps/имя.dll`; 64-битные — `programs/win64/имя.c` (`x86_64-w64-mingw32-gcc`).
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

* `make test` — собирает образ с консолью на COM-порту, запускает QEMU без окна (с сетевой картой
  и вторым FAT-диском), вводит команды из `tests/run.sh` и проверяет вывод. Новый тест = две строки
  в списке: команда и ожидаемый фрагмент. Если на хосте есть `openssl` и `python3`, тест поднимает
  свои HTTPS-серверы (TLS 1.3 и 1.2 с тестовым корневым сертификатом) и сервер cookies.
* `make test-hw` — тот же образ на 10 конфигурациях: BIOS и UEFI (нужен пакет `ovmf`), диски IDE,
  AHCI, NVMe, USB (UHCI через BIOS и свой xHCI), процессор 486; проверяются оболочка, запуск
  Windows-программы и сохранение `/home` после перезагрузки (~3 минуты).
* `make test-crypto` — собирает криптографию ядра для хоста и сверяет с эталонами из Python-пакета `cryptography`.
* `tests/shot.sh build/vga/synthos.img out.png 12 <клавиши QEMU…>` — запускает VGA-образ, нажимает
  клавиши (`l s ret`, `shift-apostrophe`, `sleep:5` — пауза) и сохраняет скриншот через
  `AFTER` секунд после последней клавиши (нужны `socat` и Python PIL; `QEMU_EXTRA` — доп. флаги).
* Отладка Windows-программ: `config w32trace on` записывает последние вызовы API; **Ctrl+T** во время
  работы программы печатает все Windows-процессы (с `--type=` у дочерних процессов Chromium) и их
  потоки: чего ждёт каждый поток (адрес вызова как `chrome.dll+0x…` и цепочку вызовов), где сейчас
  выполняется занятый поток и, при включённом `w32trace`, его последние вызовы API. В конце —
  незавершённые чтения из каналов, в которых уже лежат данные (признак потерянного пробуждения).
* `tools/w32audit.py build/serial/kernel.elf` — сверяет, сколько байт стека снимает каждая встроенная
  функция Win32 (`ret N`), с сигнатурами из `.spec`-файлов Wine (скачиваются один раз в
  `build/wine-specs`, нужен `python3-capstone`). Несовпадение ломает стек вызывающей программы.
* Скорость: `time`, `prof -o файл команда` + `tools/profsym.py`, `winbench`. Профилировщик видит
  только код, который выполняется с включёнными прерываниями; в QEMU без KVM инструкции x87 и
  переходы между страницами заметно дороже, чем на настоящем процессоре.

---

## 9. Дорожная карта (идеи, по приоритету)

1. **Многозадачность:** процессы с вытеснением, фоновые задачи (`cmd &`), `ps`/`kill`; сейчас рабочий
   стол кооперативный; долгие загрузки (обои, Chrome) сами подкачивают события стола, чтобы он не замирал.
2. **Chrome дальше:** песочница Chrome требует ограниченных токенов и внедрения в чужие процессы — этого в
   SynthOS нет, поэтому система сама запускает его с `--no-sandbox` (и без GPU-процесса); обновления
   Chrome — повторной установкой.
3. **Windows дальше:** DirectX/OpenGL, COM, реестр с данными, сервисы; драйверы `.sys` не
   поддерживаются (вместо них — свои драйверы в `arch/`).
4. **Звук дальше:** HDA, MP3/OGG, `PlaySound`/`waveOut` для Windows-программ; видео H.264.
5. **Wi-Fi и IPv6.**
6. **Язык высокого уровня** для SX (мини-C или свой скриптовый язык) с компилятором внутри ОС.
7. **Эмулятор DOS:** 386-инструкции, EMS/XMS, мышь INT 33h, графика 13h; эмуляция CMOV на i486
   для Windows-программ, собранных под i686.
8. **Порты:** ARM (Raspberry Pi), RISC-V — через новый `arch/`.

---

## 10. Для Claude в новом чате

1. Прочитать этот файл, затем `docs/ARCHITECTURE.md` и `CLAUDE.md`.
2. Проверить окружение: `make && make test` — должно быть `tests: 173 passed, 0 failed`;
   при наличии `ovmf` — `make test-hw` (`hardware tests: 29 passed, 0 failed`);
   при наличии Python-пакета `cryptography` — `make test-crypto`.
3. Визуальная проверка: `tests/shot.sh build/vga/synthos.img /tmp/shot.png 12 f e t c h ret`
   (после загрузки открыт рабочий стол, клавиши попадают в окно терминала; `launch <app>` открывает
   приложения). Мышь в QEMU можно двигать через монитор: `mouse_move dx dy`, `mouse_button 1`.
4. Соблюдать правила из раздела 7, расширять через макросы из раздела 6, добавлять тесты в
   `tests/run.sh` для всего нового. Код ядра должен оставаться меньше 3.5 МБ (`kernel_end <
   0x380000`, проверяет `linker.ld`): большие таблицы — в куче, а не в `.bss`. Ramdisk в этот лимит
   не входит: он дописывается после образа пакетом и загружается в кучу.
5. Известные ограничения: рабочий стол и программы кооперативные (пока команда или загрузка
   страницы работает, остальные окна ждут); одна программа Windows за раз (она может запускать
   другие); ядро 32-битное, 64-битный код выполняется только внутри программ Win64; эмулятор DOS —
   8086/186 и текстовый режим; браузер не загружает картинки, CSS и JavaScript; драйверы Windows
   (`.sys`) не поддерживаются.
