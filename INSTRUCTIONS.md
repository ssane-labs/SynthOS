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
* **несколько ядер процессора**: при загрузке SynthOS находит и запускает все ядра через локальный
  APIC (xAPIC/x2APIC, INIT + SIPI) — строка «Processors: N cores»; у каждого адресного пространства
  свои таблицы страниц. Пока вся работа идёт на первом ядре, остальные ждут: подсистема Windows
  держит одно «текущее» состояние процесса, и разнести потоки по ядрам — следующий шаг
  (`docs/ARCHITECTURE.md`, «Адресные пространства и процессоры»);
* **рабочий стол с четырьмя образами** (`config look mono|dune|mist|night`, Настройки → Appearance): у
  каждого свои обои (рисуются кодом в разрешении экрана, светлый и тёмный вариант), палитра и раскладка
  оболочки:
  * **Mono** (по умолчанию) — как macOS: строка меню сверху, Dock внизу по центру; чёрно-белые обои
    из «жидкого стекла» — прозрачные стеклянные ленты со светлой кромкой, бликом и тенью;
  * **Dune** — тёплое стекло, угловая панель запуска внизу слева (щелчок по «Apps» раскрывает сетку
    всех приложений), капсула статуса справа;
  * **Mist** — строка меню сверху, плавающий Dock по центру;
  * **Night** — лента приложений слева, тёмная палитра;
  * виджетов по умолчанию нет ни в одном образе — стол чистый, их добавляют сами (см. ниже);
  * **в любом образе можно настроить всё** (Настройки → Desktop & Dock, `config dockstyle
    bottom|corner|side`, `config statusbar menubar|pill`): Dock внизу, в углу или
    сбоку, строка меню или капсула статуса; у каждого образа своя раскладка
    (`~/.looks`), кнопка Reset возвращает исходную;
  * **Dock: всегда, скрывается или выключен** (Настройки → Desktop & Dock → Show the Dock,
    `config dockmode show|autohide|off`): скрытый Dock плавно уезжает вниз и выезжает, когда указатель
    касается нижнего края; выключенный не рисуется и не занимает место, приложения открываются из
    Launchpad (⌘ или Ctrl+Esc) и Spotlight;
* **окна**: «Fill Screen» (зелёная кнопка или меню ⋯) разворачивает окно ровно на свободную область —
  вплотную к строке меню и Dock, с прямыми углами; **полный экран** — F11, Ctrl+⌘+F или ⋯ → Enter Full Screen:
  окно занимает весь экран, строка меню и Dock убираются (строка меню появляется, пока указатель у верхнего
  края); из терминала — `windows max|full <n>`, `windows frame <n>` показывает, где окно;
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
  `bundle` и `bundle extract папка`. Распаковка идёт короткими шагами между событиями рабочего стола —
  пока она идёт, можно пользоваться системой. Если образ собран без него, щелчок по значку ставит
  Google Chrome из сети (ниже). Работают меню (⋮, профиль, правый щелчок), выпадающие списки,
  всплывающие окна сайтов, загрузки и выбор файлов для отправки на сайт (`<input type=file>`
  открывает панель выбора файла SynthOS);
* **Google Chrome** — по желанию (`launch Chrome`, `pkg install chrome`); образ без Chromium через
  15 секунд после первого входа (если есть сеть и место на диске) сам начинает качать Chrome в фоне — полоска под значком в Dock показывает ход,
  а когда всё готово, приходит уведомление (`config chromeauto off` — не качать самому). Можно и не ждать:
  щелчок по значку показывает то же окно с ходом загрузки. SynthOS спрашивает сервер
  обновлений Google, скачивает официальный офлайн-пакет сжатым gzip (231 МБ вместо 443 МБ, распаковка
  на лету, окно TCP до 1 МБ и подтверждения через сегмент, SHA-256 проверяется, свободное место — заранее) и распаковывает программу прямо из 7z-архива внутри установщика в `/home/apps/chrome`
  (свой распаковщик 7z: LZMA, BCJ2, команда `un7z`), кладёт ярлык на стол и открывает Chrome; то же —
  `pkg install chrome`. Нужно ~900 МБ свободного места: диск ВМ должен быть хотя бы 1,5 ГБ;
* **панель «Открыть / Сохранить»** как в macOS: слева «Избранное» (Домой, Рабочий стол, Документы,
  Загрузки, Изображения) и диски, справа папка (сначала папки, неподходящие файлы приглушены), кнопки
  «назад/вверх», поле имени при сохранении, «Новая папка» при выборе папки; ею пользуются Блокнот и
  программы Windows (`IFileOpenDialog`/`IFileSaveDialog`, `GetOpenFileName`/`GetSaveFileName`);
* приложения — Chromium, Терминал, Файлы, Блокнот, **Фото** (PNG, JPEG baseline/progressive, GIF с
  анимацией, BMP; масштаб, поворот, слайд-шоу, обои из фото), **Музыка** (встроенный синтезатор с тремя
  своими композициями и WAV-файлы), **Видео** (AVI с Motion JPEG и PCM-звуком), **Просмотр** (любой
  файл текстом или в hex), Paint, Калькулятор, Системный монитор, Сапёр, Системные настройки,
  **Установка SynthOS**; **автозапуск, как в Windows** — это папка «Автозагрузка» (`C:\Users\<имя>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup` и общая `C:\ProgramData\...\Startup`) плюс значения `Run`/`RunOnce` в `HKCU`/`HKLM`, которые пишут установщики: что положено в папку (ярлык `.lnk`, файл, `Имя.app`, `имя.command` с командой) или записано в `Run`, открывается при входе. Настройки → General → Login Items показывают и убирают всё это, `autostart add|remove|list|folder` — то же из терминала;
  композитный оконный менеджер, «жидкое стекло» любой формы, вход по паролю, блокировка экрана,
  несколько мониторов;
* **Системные настройки как в macOS**: слева поиск, карточка пользователя и панели группами (Network,
  Notifications, Sound, General, Appearance, Accessibility, Desktop & Dock, Displays, Wallpaper, Lock Screen,
  Privacy & Security, Users & Groups, Keyboard, Mouse, Terminal, Windows Apps); у General — свои страницы
  About, Storage (занятость дисков), Date & Time и Login Items; кнопки «назад/вперёд» в заголовке. Справа —
  скруглённые группы строк: название с пояснением слева, переключатель, сегменты, выпадающий список,
  ползунок или поле справа. Поиск ищет и по названиям панелей, и по каждой строке внутри: в списке остаются
  только подходящие панели, нужные строки подсвечены, Enter открывает панель, где совпадений больше всего
  (`launch Settings mouse` — сразу открыть панель). Каждая строка — обычная настройка `config`, она
  сохраняется сразу. Новые настройки:
  * часы: `config clock24 on|off` (24 или 12 часов с AM/PM), `clockseconds` (секунды в строке меню и капсуле),
    `clockdate` (дата в строке меню), `config timezone +03:00` — часовой пояс (часы компьютера идут по UTC;
    пояс прибавляется к часам на столе, экрану блокировки, виджету, `date`, `cal` и приглашению);
  * `config dnd on` — «Не беспокоить»: уведомления не всплывают;
  * звук: `config volume 0..100` и `config mute on|off` (громкость всего, что играет через AC'97);
  * клавиатура: `config keyrate 2..30` (повторов в секунду) и `config keydelay 250..1000` (мс до повтора) —
    USB-клавиатуре сразу, PS/2 — командой typematic;
  * мышь: `config doubleclick 150..1500` (мс между щелчками), `config scrollspeed 1..5`,
    `config mouseprimary left|right` (для левой руки);
  * Dock: `config dockdots on|off` (точки под открытыми приложениями), `config dockbounce on|off`
    (подскок при запуске);
  * `config browser chromium|chrome` — браузер по умолчанию для `.html` и ссылок;
  * строка состояния терминала теперь `config termstatus on` (`config statusbar` — строка меню или капсула);
* **звук**: драйвер Intel AC'97 (16 бит, стерео, кольцо DMA-буферов); в QEMU — `-device AC97`
  (`make run` добавляет его сам), команда `music`;
* **установка на компьютер** (подробно — [docs/INSTALL.md](docs/INSTALL.md)): `make usb` даёт образ
  для флешки (Rufus в режиме DD, balenaEtcher, dd); загрузка с флешки показывает «Try / Install».
  Установщик рисует карту диска и ставит систему **рядом с Windows в свободное место**, **вместо
  выбранного раздела** или **на весь диск**, с выбором размера ползунком; таблицы MBR и GPT (обе
  копии GPT с контрольными суммами) правит `kernel/core/parttab.c`, команда `part`. Рядом с другой
  системой SynthOS получает свой раздел EFI (4 МБ) и раздел системы; при первой перезагрузке
  загрузчик с флешки сам добавляет «SynthOS» в меню загрузки UEFI (`Boot####`, `BootOrder`) и
  запускает установленную систему; дальше при каждом включении — меню «SynthOS / Windows Boot
  Manager» (Windows запускается штатно через `BootNext`, BitLocker не замечает разницы).
  Копия системы находится по 64-битному id в загрузочной записи, поэтому флешка и установленная
  система не путаются;
* **мастер первого запуска**: раскладки клавиатуры и часовой пояс, учётная запись с паролем и
  автовходом, оформление (Dune/Mist/Night, светлое/тёмное, акцент — меняется сразу за карточкой),
  браузер (Chromium или Google Chrome); `welcome` открывает его снова;
* **раскладки клавиатуры**: английская, русская, немецкая, французская, испанская; переключение
  Win+Space или щелчком по EN/RU в строке состояния; ввод в поля, Блокнот и Windows-программы
  (`WM_CHAR` с Юникодом); `config keyboard us,ru`;
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
* **любой браузер семейства Chromium или Firefox**: движок узнаётся не по имени программы, а по файлам рядом
  с ней — `xul.dll` значит Gecko (Firefox, LibreWolf, Waterfox, Floorp, Zen, Tor/Mullvad Browser),
  `chrome.dll`, `*_elf.dll`, `*_proxy.exe` или `libcef.dll` — Chromium (Chrome, Edge, Brave, Vivaldi, Opera,
  Яндекс Браузер, ИИ-браузеры вроде Perplexity Comet, Thorium, ungoogled-chromium). Chromium-браузеры получают
  те же ключи, что Chrome; для Gecko система сама задаёт `MOZ_DISABLE_*_SANDBOX=1` (песочнице Gecko нужны
  ограниченные токены, которых в SynthOS нет). **Firefox** (32-битный) запускается с процессом-лаунчером,
  отдельными контент-процессами и общим кэшем стилей; в режиме `firefox.exe --headless --screenshot
  C:\Users\<имя>\s.png file:///C:/путь/страница.html` сохраняет снимок страницы. Свои шрифты браузера
  (например, `TwemojiMozilla.ttf`) DirectWrite открывает по пути;
* **установка любых программ «как на Windows»**: скачанный установщик (`.exe` из браузера или с флешки)
  запускается двойным щелчком и ставит программу сам — система ничего не знает о конкретных программах.
  Работают 7-Zip SFX, NSIS (в том числе плагины `System`, `nsExec`, INI-файлы), C++-исключения MSVC
  (`_CxxThrowException`, `__CxxFrameHandler3`), `ShellExecuteEx` с дескриптором процесса, папки меню «Пуск»
  (`CSIDL_PROGRAMS`, `CSIDL_COMMON_PROGRAMS`, PIDL). Что установщик записал, там и используется: ярлыки
  из меню «Пуск» (`C:\ProgramData\Microsoft\Windows\Start Menu\Programs` и пользовательского) появляются
  в Launchpad и Spotlight со своими значками, ярлык на рабочем столе — на рабочем столе, записи
  `...\CurrentVersion\Uninstall` — в «Системных настройках → Windows Apps → Installed Programs» с кнопкой
  Uninstall. Документы и ссылки открываются программой, которую назначил реестр (`UserChoice`,
  `HKCR\.ext` → ProgID → `shell\open\command`, `HKCR\<схема>`), `AssocQueryString` отвечает так же.
  Браузеры, зарегистрированные в `SOFTWARE\Clients\StartMenuInternet`, появляются в списке «Браузер по
  умолчанию» (настройки или `config browser <ключ>`): выбор записывается в `UserChoice` для http/https/.html,
  как в Windows, и ссылки с рабочего стола и из программ открываются в нём; `ms-settings:defaultapps`
  (кнопка «Сделать браузером по умолчанию» в самом браузере) открывает эти настройки. Проверено на
  официальном установщике **Firefox**: `ffsetup.exe /S` ставит Firefox в `C:\Program Files\Mozilla Firefox`,
  регистрирует его (`regsvr32`, служба обслуживания, `StartMenuInternet`, `Uninstall`). Окно Firefox на рабочем
  столе рисует вкладки, адресную строку и страницу (WebRender без видеокарты выводит кадры через
  `StretchDIBits`), а также безголовые снимки (`--screenshot`). Под эмуляцией без KVM первый запуск занимает
  минуты, на настоящем компьютере — заметно быстрее. Дочерние процессы программ, запущенных с рабочего стола,
  живут после выхода родителя;
* **MSI-пакеты** (`.msi`): `msiexec /i пакет.msi [/qn] [СВОЙСТВО=значение]` ставит файлы из встроенного
  cabinet-архива (MSZIP), ярлыки и значения реестра, записывает запись `Uninstall` и список записанного в
  `C:\Windows\Installer`; `msiexec /x {код продукта}` удаляет ровно записанное. `.msi` открывается через
  `msiexec` (ассоциация в реестре). Пользовательские действия (custom actions) и службы не выполняются —
  установщик сообщает, сколько их пропущено; установка обычным Windows Installer (с откатом) не эмулируется;
* **Steam**: официальный `SteamSetup.exe` (установщик NSIS) ставит Steam в `C:\Program Files\Steam`
  (тихо — `SteamSetup.exe /S`, или обычным окном); `steam.exe` открывает окно обновления, по HTTPS
  (своя проверка цепочки сертификатов Windows — `CertGetCertificateChain`) скачивает клиент с
  серверов Valve (~240 МБ), распаковывает и ставит его. С 2026 года клиент Steam только 64-битный,
  поэтому 32-битные программы видят 64-битную Windows (WOW64), если процессор умеет 64-битный режим
  (`config w32wow64 on`, по умолчанию). Нужно ~2 ГБ свободного места: диск ВМ — от 4 ГБ. Служба Steam
  (`steamservice.exe /Install`) не регистрируется — диспетчера служб в SynthOS нет, установщик это
  пропускает. После обновления 64-битный `steam.exe` запускается, инициализирует свои библиотеки
  (`tier0_s64`, `steamui`, `vgui2_s`, кодеки…) и запускает `steamwebhelper.exe` — Chromium (CEF).
  Steam и его процессы Chromium работают одновременно и общаются через именованные события и общую
  память; появляется первое окно Steam, но интерфейс (WebUI) пока не загружается. Для этого 64-битные программы получили адреса выше 4 ГБ
  (Chromium резервирует пулы по 16 ГБ), 64-битный мост к встроенному DirectWrite и правильную
  передачу 8-байтных указателей и дескрипторов в функции Windows и обратно. **Counter-Strike 2** не запустится: ему нужен Direct3D 11/Vulkan
  на настоящей видеокарте, а SynthOS рисует только в кадровый буфер;
* **Python для Windows** (официальные сборки 3.12, 32- и 64-битные): `python.exe` запускается, работают
  `json`, `hashlib`, `ctypes` (в том числе `python3.dll`), `os`, `sys`. **Git для Windows** (MinGit):
  `git --version`, `init`, `add`, `commit`; `git log` без пейджера — в MinGit нет `less`
  (`git --no-pager log` или `git config core.pager ""`);
* что для этого появилось в подсистеме Windows: INI-файлы (`Get/WritePrivateProfileString`),
  именованные события/мьютексы/семафоры (`ERROR_ALREADY_EXISTS`, `OpenMutex`…), `SearchPath`,
  `SHFileOperation`, `CopyFileEx`, полосы прогресса (`msctls_progress32`), `IsBadReadPtr`; файл
  **hosts** — `/home/etc/hosts` (он же `C:\Windows\System32\drivers\etc\hosts`), читается раньше
  DNS; **длинные имена файлов** в SynthFS — до 255 байт (старые диски читаются как прежде);
  поиск имени без учёта регистра в больших папках — один проход (1000 файлов в папке создаются
  за 2,1 с вместо >400 с); программа, чей файл заменили, пока она работает (так обновляется
  Steam), доигрывает со старым кодом; однопоточная программа, ждущая дочернюю через `Sleep`,
  больше не мешает ей работать; удаление файлов во время `FindFirstFile/FindNextFile` ничего не
  пропускает; `LoadLibrary` внутри `DllMain` запускает только то, что сам загрузил (как в Windows);
* пользователи с паролями (`useradd`, `passwd`, `su`, `lock`), у каждого свой `/home`;
* виртуальная машина **SX** для своих программ + ассемблер/дизассемблер внутри ОС;
* эмулятор **MS-DOS** (8086/186) для `.COM` и `.EXE`;
* автотесты: `make test` (302 проверки), `make test-hw` (BIOS/UEFI × IDE/AHCI/NVMe/USB/xHCI, i486;
  48 проверок), `make test-crypto` (криптография против эталонов).

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

**64-битное ядро.** Тот же исходный код собирается как 64-битное ядро (x86-64, long mode):

```sh
make ARCH=x86_64                 # build/vga64/synthos.img
make ARCH=x86_64 run             # и любые другие цели: test, test-hw, usb, run-uefi
```

Загружается теми же загрузчиками (BIOS и UEFI, Secure Boot), видит всю память через 4-уровневые таблицы
страниц, запускает все ядра процессора; работают рабочий стол, диски (IDE/AHCI/NVMe/USB), сеть, установщик,
вызовы BIOS (диск и видеорежимы VESA на старых компьютерах). Нужен процессор x86-64 (i486 остаётся за 32-битной
сборкой). **32-битные программы Windows работают** и здесь: подсистема Windows собирается как код x32
(64-битный режим процессора с 4-байтными указателями — та же раскладка структур, что у 32-битных программ),
программы выполняются в режиме совместимости и вызывают встроенные функции через шлюз. **64-битные программы
Windows (PE32+) на 64-битном ядре пока не запускаются** — это следующий этап, поэтому основной образ остаётся
32-битным. `make test ARCH=x86_64` пропускает проверки 64-битных программ.

Полезное:

* Выход из QEMU: команда `shutdown` внутри SynthOS (или закрыть окно; в `run-serial` — `Ctrl+A`, затем `X`).
* Файлы в `/home` хранятся в самом образе. Чтобы начать «с чистого диска» — `make clean && make`.
* Отключение подсистем для маленьких машин (`config.mk` или командная строка):
  `make CONFIG_DOS=0 CONFIG_SXVM=0 CONFIG_EDITOR=0`.
* Дополнительные флаги QEMU: `make run QEMU_FLAGS="-smp 2 -device qemu-xhci -device usb-kbd"`.
* Сеть в QEMU работает сразу (user-mode, адрес по DHCP; загрузка его не ждёт — строка
  «Network: … getting an address», первая сетевая команда сама дождётся адреса). Сертификаты HTTPS проверяются по корням
  из `/etc/certs` (генерируются `tools/mkcerts.sh`); свои корни — файлы `.pem` в `/home/certs`
  и команда `certs reload`.

### На реальном компьютере

Пошаговая инструкция с установкой рядом с Windows — **[docs/INSTALL.md](docs/INSTALL.md)**.

1. `make usb` и записать `build/synthos-usb.img` на флешку **побайтно** (все данные на флешке пропадут):
   * Linux/WSL: `sudo dd if=build/synthos-usb.img of=/dev/sdX bs=4M status=progress conv=fsync`
   * Windows: Rufus → выбрать `synthos-usb.img` → режим **DD-образ** (или balenaEtcher).
2. Загрузиться с флешки через меню загрузки (обычно F12, F11, F8 или Esc). Подходит и
   **UEFI**, и **Legacy/CSM** — образ гибридный. **Secure Boot** можно оставить включённым, если добавить
   в прошивку свой ключ (см. ниже), или выключить.
3. На экране «Welcome to SynthOS» — **Install SynthOS**, выбрать диск и место, перезагрузиться,
   не вынимая флешку, и пройти мастер первого запуска.

Где сохраняется `/home`: на диске, с которого загрузилась система, если это IDE, SATA (AHCI)
или NVMe; при загрузке через BIOS — на любом диске, который видит BIOS (включая USB-флешку).
При загрузке через UEFI с USB-флешки `/home` хранится на ней же через свой драйвер xHCI.
Чужие диски система не трогает: раздел используется, только если на диске лежит именно
загрузочный образ SynthOS (сигнатура в MBR + заголовок ядра).

**Secure Boot.** Подписи Microsoft у SynthOS нет, поэтому на компьютере с заводскими ключами загрузчик
SynthOS запустится только после того, как в прошивку добавлен ваш ключ. При первой сборке `make` создаёт
ключ в `keys/` (`db.key` держите в тайне, в git он не попадает, `make clean` его не удаляет), подписывает
им UEFI-загрузчик и кладёт сертификат `SYNTHOS\SYNTHOS.CER` на EFI-раздел флешки и установленной системы.
Ключ Microsoft остаётся в прошивке — Windows грузится как прежде. Загрузчик сам проверяет ядро: при
включённом Secure Boot он запускает только тот `KERNEL.BIN`, SHA-256 которого вписан в него при сборке, так
что подменить ядро без нового подписанного загрузчика нельзя. Свой ключ — `make SB_KEY=… SB_CERT=…`,
без подписи — `make SB=0`. Проверено в `make test-hw` на OVMF с Secure Boot: подписанный загрузчик
стартует, чужой прошивка не запускает («Access Denied»), изменённое ядро загрузчик не запускает.

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
config look night         образ рабочего стола: mono, dune, mist, night (или Настройки → Appearance)
music, install, autostart музыка, установка (рядом с Windows / вместо раздела / на весь диск), программы при входе
start file.msi            открыть файл или ссылку программой, назначенной в реестре (как start в Windows)
part [диск]               таблица разделов; init gpt|mbr, add <МБ> [тип] [имя], del <n> (с --yes)
welcome                   мастер первого запуска (раскладки, учётная запись, оформление, браузер)
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
адреса переводятся в функции `tools/profsym.py build/vga/kernel.elf < файл`), `prof start` и
`prof -k stop` (профиль в фоне, например пока открывается Chromium; отчёт в журнал ядра), `prof -c`
(время ядра у вызывающей функции), `schedstat [секунды]` (переключения и ожидания потоков Windows),
`winbench [папка]` (файлы, консоль, куча, вызовы API), `bench`.

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
                          menubar.c, clock.c (вид часов), dock.c (центр, угол, лента), panel.c (статус), widgets.c (виджеты, погода), media.c (синтезатор,
                          WAV, команда music), autostart.c, spotlight.c (поиск, Launchpad), lock.c,
                          wallpaper.c (бумажные обои из слоёв), apps/chromium.c (предустановленный браузер), ui.c, terminal.c, image.c; uifont.c и icons.c —
                          сгенерированы
kernel/gui/apps/          browser, files, notepad, photos, music, video, viewer, installer, paint,
                          calc_fp, settings, monitor, mines
kernel/lib/               png.c, jpeg.c, gif.c, picture.c (BMP и общий вход), inflate.c, zip.c, lz4.c, …
kernel/core/install.c     установка: весь диск, свободное место или вместо раздела (команда install)
kernel/core/parttab.c     таблицы разделов MBR и GPT: карта диска, добавить/удалить раздел (команда part)
kernel/core/keymap.c      раскладки клавиатуры (config keyboard, Win+Space)
kernel/gui/setup.c        мастер первого запуска и экран «Try / Install» при загрузке с флешки
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
* `make test-hw` — тот же образ на 12 конфигурациях: BIOS и UEFI (нужен пакет `ovmf`), диски IDE,
  AHCI, NVMe, USB (UHCI через BIOS и свой xHCI), процессор 486, 4 ядра; проверяются оболочка, запуск
  Windows-программы и сохранение `/home` после перезагрузки. Затем установка: рядом с разделом
  «Windows» на GPT-диске под UEFI (переменные UEFI в копии `OVMF_VARS`: флешка регистрирует
  систему, система грузится с диска, потом без флешки через свой пункт меню) и на весь диск под
  BIOS (~4 минуты). `tools/gptcheck.py диск.img` проверяет обе копии GPT и контрольные суммы.
* `make test-crypto` — собирает криптографию ядра для хоста и сверяет с эталонами из Python-пакета `cryptography`.
* `tests/shot.sh build/vga/synthos.img out.png 12 <клавиши QEMU…>` — запускает VGA-образ, нажимает
  клавиши (`l s ret`, `shift-apostrophe`, `sleep:5` — пауза) и сохраняет скриншот через
  `AFTER` секунд после последней клавиши (нужны `socat` и Python PIL; `QEMU_EXTRA` — доп. флаги).
* Отладка Windows-программ: `config w32errors on` пишет в журнал каждый вызов API, закончившийся
  ошибкой (с именем функции), неудачные открытия и переименования файлов и `GetProcAddress`;
  `config w32trace on` записывает последние вызовы API; **Ctrl+T** во время
  работы программы печатает все Windows-процессы (с `--type=` у дочерних процессов Chromium) и их
  потоки: чего ждёт каждый поток (адрес вызова как `chrome.dll+0x…` и цепочку вызовов), где сейчас
  выполняется занятый поток и, при включённом `w32trace`, его последние вызовы API. В конце —
  незавершённые чтения из каналов, в которых уже лежат данные (признак потерянного пробуждения).
  На рабочем столе то же печатает команда `w32ps`. `config logserial on` копирует журнал ядра в COM1
  (в QEMU — `-serial file:log.txt`): там видно каждое созданное окно (класс, стили), завершение
  процессов с кодом, а программы, запущенные с рабочего стола, пишут туда свой stderr — например,
  `launch Chromium --enable-logging=stderr` даёт журнал самого Chromium.
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
2. **Браузеры дальше:** песочницы Chrome и Firefox требуют ограниченных токенов и внедрения в чужие
   процессы — этого в SynthOS нет, поэтому система сама запускает Chromium-браузеры с `--no-sandbox` (и без
   GPU-процесса), а Gecko — с `MOZ_DISABLE_*_SANDBOX`; обновления — повторной установкой. Установка любых браузеров их
   собственными установщиками и выбор браузера по умолчанию через реестр сделаны; дальше — скорость
   Firefox (под эмуляцией без KVM первый запуск занимает минуты), custom actions
   MSI-пакетов и службы.
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
