# Как помочь проекту / Contributing

Спасибо за интерес! Ниже — коротко, как собрать, проверить и прислать изменения.
Thanks for your interest! Below is a short guide to building, testing and sending changes (English after the Russian).

## По-русски

1. **Сборка.** ESP-IDF v6.1, затем `idf.py build` и `idf.py -p <PORT> flash` (на Windows — `.\tools\idf.ps1 build`,
   `.\tools\flash.ps1`). Шрифт CJK — один раз `idf.py -p <PORT> storage-flash`. Подробнее — в [README](README.md#быстрый-старт).
2. **Проверка на плате.** `python tools/smoke.py` (с `--no-ai` — без платных запросов к OpenRouter).
   Для интерфейса — скриншоты: `python tools/dev.py shot shots/<имя>.png`, проверка вписывания строк: `python tools/dev.py ui fit`.
3. **Тесты без платы.** `python tools/test_dev.py`; host-тесты модулей (Windows + MSVC Build Tools):
   `powershell -NoProfile -ExecutionPolicy Bypass -File components\ai\host_test\build.ps1` и так же для
   `components\{audio,board,devconsole}`, `main\app`, `main\ui`. Скрипт собирает `out\*.exe` — запустите его
   (например, `components\ai\host_test\out\host_test.exe`) и ждите `N checks, 0 failures`; `main\ui` запускает
   свои тесты сам. Для `ai` и `main\ui` сначала нужен один `idf.py build`.
4. **Стиль кода.** C: 4 пробела, без табов, строки до ~120 символов, комментарии на английском со ссылками на
   разделы `docs/` (например, `// ui.md 2.3`). Python: 4 пробела, только стандартная библиотека (+ `pyserial` для
   `dev.py`); исключения — генераторы шрифтов: `tools/mkcjk.py` нужен fontTools (`pip install fonttools==4.67.0`),
   `tools/mkfonts.ps1` — Node.js и `lv_font_conv` через `npx` (`tools/package.json`). Настройки редактора — в [.editorconfig](.editorconfig).
5. **Строки интерфейса** — в `main/ui/ui_str.c`, всегда на двух языках (RU и EN), и обязательно должны вписываться в круг.
6. **Поведение меняется — обновите документацию** в `docs/` (`ui.md`, `protocol.md`, `architecture.md`).
7. **Коммиты** — короткая строка в стиле `компонент: что сделано` (например, `ui_rim: fix freeze when …`).
8. **Никаких секретов** в коммитах, логах и скриншотах — см. [SECURITY.md](SECURITY.md).

## In English

1. **Build.** ESP-IDF v6.1, then `idf.py build` and `idf.py -p <PORT> flash` (on Windows: `.\tools\idf.ps1 build`,
   `.\tools\flash.ps1`). The CJK font goes once with `idf.py -p <PORT> storage-flash`. See the [README](README.en.md#quick-start).
2. **Test on the board.** `python tools/smoke.py` (`--no-ai` skips the paid OpenRouter requests).
   For UI work: screenshots with `python tools/dev.py shot shots/<name>.png`, the string-fit check `python tools/dev.py ui fit`.
3. **Tests without a board.** `python tools/test_dev.py`; module host tests (Windows + MSVC Build Tools):
   `powershell -NoProfile -ExecutionPolicy Bypass -File components\ai\host_test\build.ps1`, and the same for
   `components\{audio,board,devconsole}`, `main\app`, `main\ui`. The script compiles `out\*.exe`: run it (e.g.
   `components\ai\host_test\out\host_test.exe`) and expect `N checks, 0 failures`; `main\ui` runs its tests itself.
   `ai` and `main\ui` need one `idf.py build` first.
4. **Code style.** C: 4 spaces, no tabs, lines up to ~120 columns, comments in English that point to the `docs/`
   sections (e.g. `// ui.md 2.3`). Python: 4 spaces, standard library only (+ `pyserial` for `dev.py`),
   except the font generators: `tools/mkcjk.py` needs fontTools (`pip install fonttools==4.67.0`) and
   `tools/mkfonts.ps1` needs Node.js with `lv_font_conv` via `npx` (`tools/package.json`).
   Editor settings are in [.editorconfig](.editorconfig).
5. **UI strings** live in `main/ui/ui_str.c`, always in both languages (RU and EN), and must fit inside the circle.
6. **Behavior changes need doc updates** in `docs/` (`ui.md`, `protocol.md`, `architecture.md`).
7. **Commits**: one short line, `component: what changed` (e.g. `ui_rim: fix freeze when …`).
8. **No secrets** in commits, logs or screenshots — see [SECURITY.md](SECURITY.md).
