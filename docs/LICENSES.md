# Лицензии: проверка

Проверено всё, что лежит в репозитории и попадает в образы (`make`, `make usb`, `make windows`). Нарушений не
найдено; недоставало только текстов лицензий шрифтов внутри системы — теперь они в `/system/LICENSES.txt`
(Настройки → Основные → Об этой системе → Licenses).

## Своё

Ядро, загрузчики (`arch/x86/boot`, `arch/x86/efi`), рабочий стол, подсистема Windows (`kernel/win32`, в том числе
`msvcp60.dll` из `programs/win32/dll/msvcp60.c`), сжатие (LZ4, deflate/inflate), криптография, PNG/JPEG/GIF/AVI, FLAC/AIFF/TIFF/TGA/QOI/ICO,
значки (`tools/mkicons.py` рисует их кодом), музыка (`kernel/gui/media.c` синтезирует), видео-образец
`Sunset.avi` (нарисован программно), тестовые программы в `programs/*/prebuilt` (собраны из исходников рядом).
Чужой код в ядро не копировался; `.spec` из Wine `tools/w32audit.py` только читает на машине разработчика, в образ
они не попадают.

## Чужое, что едет с системой

| Что | Где | Лицензия | Что требует | Сделано |
|-----|-----|----------|-------------|---------|
| Inter | шрифты стола (`uifont.c`, `ui.lz`) | SIL OFL 1.1 | текст лицензии и копирайт в каждой копии | `/system/LICENSES.txt`, заголовок `uifont.c` |
| DejaVu | `DejaVuSans.ttf` (подмножество), консольный и моноширинный шрифты | Bitstream Vera + public domain | текст лицензии; изменённый шрифт не называть «Bitstream»/«Vera» | текст в `/system/LICENSES.txt`; имя — DejaVu |
| Корневые сертификаты Mozilla | `/etc/certs/mozilla.pem` | MPL 2.0 | указать лицензию и где взять исходник | `/system/LICENSES.txt` (файл и есть исходник) |
| Фотографии NASA | `/system/wallpapers` | общественное достояние (США) | NASA просит указывать авторство и не изображать одобрение | `/system/wallpapers/CREDITS.txt` |
| shim (Ubuntu, подписан Microsoft) | раздел EFI | BSD 2-clause | копирайт | `\SYNTHOS\SHIM.TXT` на разделе EFI |
| Chromium | только образ с рабочим столом, `/home/apps/chromium` | BSD 3-clause + лицензии частей | копирайт и условия | текст BSD в `/system/LICENSES.txt`, список частей — `chrome://credits` |
| Obscura (движок браузера) | только 64-битный образ с рабочим столом, `/home/apps/obscura` | Apache-2.0 | копия лицензии, файл NOTICE (если есть), пометки в изменённых файлах | полный текст Apache-2.0 в `/system/LICENSES.txt`; NOTICE у проекта нет; файлы Obscura не меняются (официальный релиз v0.2.2), окно браузера — свой код (`kernel/gui/apps/obscura.c`), с движком говорит по MCP через канал |
| minimp3 | модуль `media` (`kernel/gui/optional/media/minimp3.h`), MP3 | CC0 1.0 | ничего | упомянут в `/system/LICENSES.txt` |
| stb_vorbis | модуль `media`, Ogg Vorbis | MIT или public domain (на выбор) | при MIT — копирайт и текст лицензии | взят по MIT, текст в `/system/LICENSES.txt` |
| simplewebp | модуль `media`, WebP (с потерями и без) | BSD 3-clause (код libwebp) + патентная оговорка WebM | копирайт, условия и отказ от гарантий в документации | полный текст в `/system/LICENSES.txt`; в файлах заменены только `#include`, это отмечено в их начале |

## Что пользователь скачивает сам

| Что | Откуда | Лицензия | Почему можно |
|-----|--------|----------|--------------|
| GNU Unifont 15.1.05 (`unifont.hex.gz`) | unifoundry.com, по кнопке Download или `languages install` | SIL OFL 1.1 или GPL 2+ с исключением для шрифтов | с SynthOS не едет; скачивается без изменений в `/home/fonts/cjk`, исключение для шрифтов разрешает рисовать им любой текст |
| Unihan 15.1 (`Unihan.zip`) | unicode.org, вместе со шрифтом | Unicode License v3 | с SynthOS не едет; из него на компьютере пользователя строится словарь `ime.dat`; лицензия разрешает и распространение с уведомлением |
| MiniCPM5-2B, Q4_K_M (`MiniCPM5-2B-Q4_K_M.gguf`, 1,5 ГБ) | huggingface.co (openbmb), по кнопке в окне «Assistant» или `ai get minicpm` | Apache-2.0 | с SynthOS не едет; скачивается без изменений в `/home/models`; читает и считает свой код (`kernel/gui/optional/assistant/`) |
| SmolLM2-135M-Instruct, Q4_K_M (`SmolLM2-135M-Instruct-Q4_K_M.gguf`, 105 МБ) | huggingface.co (bartowski, квантование модели HuggingFaceTB), `ai get keys` | Apache-2.0 | с SynthOS не едет; модель для автодополнения текста по Tab |
| Qwen2.5-Coder-1.5B-Instruct, Q4_K_M (`qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`, 1,1 ГБ) | huggingface.co (Qwen), `ai get coder` | Apache-2.0 | с SynthOS не едет; автодополнение кода |
| StarCoder2-3B, Q4_K_M (`starcoder2-3b-Q4_K_M.gguf`, 1,8 ГБ) | huggingface.co (second-state, квантование модели BigCode), `ai get coder-big` | BigCode OpenRAIL-M | с SynthOS не едет; скачивается только по команде; автодополнение кода на мощных ПК |
| Qwen2.5-0.5B-Instruct, Q4_K_M (`qwen2.5-0.5b-instruct-q4_k_m.gguf`, 470 МБ) | huggingface.co (Qwen), по кнопке в окне «Assistant» или `ai get` | Apache-2.0 | с SynthOS не едет; скачивается без изменений в `/home/models`; код, который его читает и считает, — свой (`kernel/gui/optional/assistant/`) |
| Whisper base, Q8_0 (`ggml-base-q8_0.bin`, 78 МБ) | huggingface.co (ggerganov/whisper.cpp), по щелчку по микрофону или `ai get speech` | MIT | с SynthOS не едет; скачивается без изменений; разбор формата и распознавание написаны заново (`assistant/whisper.h`) |

## Магазин

Программы магазина не входят в SynthOS: кнопка GET скачивает их с серверов авторов, без изменений, на компьютер
пользователя. Распространением это не является, поэтому годятся и программы, которые запрещают выкладывать их у
себя (Sysinternals, Google Chrome). Каталог (`rootfs/system/store/catalog.txt`) содержит только ссылки и описания.

## На что обратить внимание дальше

* **Пункт 14 (значки «точь-в-точь как в macOS»)**: значки Apple защищены авторским правом, копировать их один в
  один нельзя. Делать значки в том же стиле (форма, свет, материалы), но свои рисунки.
* **Пункт 15** — то же для внешнего вида браузера Dia: повторять расположение и стиль можно, логотипы и
  фирменные картинки — нет.
* **Obscura (Apache-2.0)**: если когда-нибудь собирать движок из своих изменённых исходников, в изменённых файлах
  нужно оставить пометку об изменениях, а NOTICE (если появится у проекта) класть рядом.
* У самой SynthOS нет файла `LICENSE`: пока это «все права защищены». Лицензию проекта выбирает владелец.
* Ключ Secure Boot (`keys/db.key`) в репозиторий не попадает (`.gitignore`) — так и должно остаться.
