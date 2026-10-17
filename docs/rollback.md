# Откат и восстановление прошивки (runbook)

Плата: Waveshare ESP32-S3-Touch-LCD-1.85, USB-Serial-JTAG на **COM7**. Все команды — из корня
репозитория в PowerShell. Порт держит только одна программа: перед прошивкой закройте `monitor`
и `dev.py`. Архитектура: `docs/architecture.md` §5, §8.

## Что где лежит

| Что | Где | Переживает |
|---|---|---|
| Загрузчик (с хуком power_latch: GPIO7 = 1 сразу после ROM) | 0x0 | всё, кроме полной прошивки / factory |
| Таблица разделов | 0x8000 | меняется только factory-образом |
| NVS (настройки `cfg`, сети `wifi`, служебное `sys`) | 0x9000, 24 КБ | `flash`, `app-flash`, OTA, `restore.ps1 -Tag` |
| otadata (какой слот грузить + состояние образа) | 0xF000 | OTA меняет; `idf.py flash` сбрасывает на ota_0 |
| Слоты `ota_0` / `ota_1` | 0x20000 / 0x420000, по 4 МБ | другой слот не трогается до следующего OTA |
| Заводская прошивка (полный дамп 16 МБ) | `backups\factory_full_16MB.bin` (не в git) | — |
| Архивы релизов | `releases\<tag>\` (бинарники не в git, `manifest.json` / `SHA256SUMS.txt` / `flash_args` в git) | — |

Служебные ключи NVS `sys`: `boot_cnt` (счётчик загрузок, пишется самотестом), `ok_ver` (последняя
версия, прошедшая самотест), `rb_reason` (почему самотест провалился), `rb_cnt` (сколько раз
был откат). Смотреть: `python tools\dev.py ota info`.

## Быстрые ответы

| Ситуация | Действие |
|---|---|
| Майлстоун готов | `.\tools\release.ps1 -Tag m3` (чистое дерево, `git tag -a`, сборка, архив). Затем закоммитить `releases\m3\manifest.json` |
| Вернуться к тегу из исходников | `git switch --detach m2` → `.\tools\idf.ps1 build` → `.\tools\flash.ps1` |
| Вернуться к тегу без сборки | `.\tools\restore.ps1 -Tag m2` (NVS сохраняется; `-EraseNvs` — сбросить настройки) |
| Попробовать сборку, не теряя текущую | `.\tools\slot.ps1 -Write build\round_assistant.bin -Slot 1 -Boot`; назад: `.\tools\slot.ps1 -Boot 0` |
| Проверить настоящий OTA и автоматический откат | `python tools\dev.py ota push build\round_assistant.bin` |
| Обновиться по сети | `python tools\dev.py ota url https://…/round_assistant.bin` (или `set ota_url …`, затем `ota url`) |
| Новая прошивка работает плохо, но запустилась | `python tools\dev.py ota rollback` (если предыдущий слот валиден) |
| Настройки испорчены | `python tools\dev.py settings reset`, или `.\tools\restore.ps1 -Tag <t> -EraseNvs`, или `parttool.py -p COM7 erase_partition --partition-name=nvs`; затем `python tools\secrets_push.py secrets.json` |
| Падение — разобрать | `.\tools\idf.ps1 -p COM7 coredump-info` (раздел coredump 128 КБ) |
| Прошивка сломана, но USB виден | `.\tools\flash.ps1` (esptool сам сбросит чип), или `python tools\dev.py reboot dl` → прошивать |
| Ничего не отвечает / boot loop | Режим загрузки: **держать BOOT, нажать и отпустить RESET, отпустить BOOT**. Появится ROM USB-Serial-JTAG (VID 0x303A). Затем `.\tools\flash.ps1 -NoWait` или `.\tools\restore.ps1 -Tag <t>` |
| Вернуть заводскую прошивку | `.\tools\restore.ps1 -Factory` (проверяет SHA-256, спрашивает YES). Заменяет таблицу разделов и NVS. Обратно: полный `.\tools\flash.ps1` и заново секреты |

## Как работает автоматический откат

1. `ota push` (через консоль) или `ota url` (HTTPS) пишут образ в **неактивный** слот
   (`esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES)`, стирание по секторам; CRC-32 всего образа
   сверяется в конце, затем `esp_ota_end` проверяет сам образ) и выбирают его для загрузки.
2. После перезагрузки загрузчик запускает новый образ в состоянии **PENDING_VERIFY**.
3. `app_main` запускает самотест (`main/selftest.c`, §8.2) под 10-секундным сторожем:
   NVS (запись и чтение `sys/boot_cnt`), TCA9554, дисплей, живой LVGL (кадр + 10 тиков таймера
   за 500 мс), I2S, `esp_wifi_init`, пороги памяти (internal ≥ 40 КБ, блок ≥ 16 КБ, DMA ≥ 12 КБ,
   PSRAM ≥ 3 МБ), задача консоли. Предупреждения (не фатально): неизвестный ID панели, нет тача.
4. **Успех:** `esp_ota_mark_app_valid_cancel_rollback()`, в `sys/ok_ver` пишется версия,
   событие `@! EVT {"ev":"selftest","ok":true,…}`, затем `@! READY`.
5. **Провал:** в `sys/rb_reason` пишется список проваленных проверок, `rb_cnt += 1`,
   `esp_ota_mark_app_invalid_rollback_and_reboot()` — грузится предыдущий слот.
6. **Падение или зависание до подтверждения** (panic, watchdog, сторож самотеста): при
   следующей загрузке загрузчик видит образ всё ещё в PENDING_VERIFY, помечает его ABORTED и
   грузит предыдущий слот.

Проверка после отката: `python tools\dev.py ota info` → `running` = прежний слот, `rb_reason`
заполнен; `python tools\dev.py info` → `selftest`, `ota_state`.

Тестовые сборки для проверки отката (§10, тесты 6.2 и 6.3): `CONFIG_RA_SELFTEST_FORCE_FAIL=y`
(самотест всегда проваливается) и `CONFIG_RA_SELFTEST_FORCE_PANIC=y` (падение до подтверждения),
символы в `components/ota/Kconfig`. Собирать в отдельный каталог, например
`.\tools\idf.ps1 -B build-ff -D SDKCONFIG=build-ff/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;tests/sdkconfig.forcefail" build`,
затем `python tools\dev.py ota push build-ff\round_assistant.bin`.

## Безопасный режим

Счётчик падений лежит в RTC-памяти (`RTC_NOINIT`, переживает программный сброс и panic).
Если 3 загрузки подряд упали (panic / watchdog), не дойдя до `@! READY`, и образ **не**
PENDING_VERIFY (тот случай откатывает загрузчик), прошивка стартует в безопасном режиме: только
консоль и дисплей, без Wi-Fi и звука. В `@! READY` будет `"safe_mode":true`, в `info` —
`safe_mode` и `boot_crashes`. Работают команды, не требующие подсистем: `info`, `heap`, `tasks`,
`log`, `get`/`set`/`settings`, `ota *`, `reboot`. Отсюда можно сбросить настройки, откатить слот
или залить исправленную прошивку. Счётчик обнуляется при `@! READY` и при включении питания.

## Тонкости

- **`flash` и `app-flash`.** `idf.py flash` пишет `ota_data_initial.bin` в 0xF000, поэтому после
  него всегда грузится `ota_0`. `app-flash` пишет только `ota_0`: если otadata выбирает `ota_1`,
  эффекта не будет (`info` покажет `part=ota_1`). Тогда: `.\tools\slot.ps1 -Boot 0` или полный flash.
- **`otatool switch_ota_partition`** переписывает только номер последовательности и CRC в одной
  копии otadata и оставляет её старое поле состояния. Состояние UNDEFINED считается валидным, а
  оставшееся ABORTED/INVALID заставит загрузчик проигнорировать переключение — поэтому
  `slot.ps1 -Boot` сначала делает `erase_otadata`.
- **Образы, прошитые по USB, никогда не PENDING_VERIFY**: автоматический откат защищает только
  образы, записанные через `esp_ota_*` (`ota push`, `ota url`). Если такой образ не прошёл
  самотест, он продолжает работать для отладки (`selftest=fail` в `info`).
- **Первый OTA после `idf.py flash`.** После полной прошивки otadata пуст, и у работающего слота
  нет записи (`ota info` → `state=undefined`). Перед первым `ota push` / `ota url` прошивка сама
  записывает работающий слот в otadata как VALID — иначе откатываться было бы некуда
  (`rollback_possible=false`, а провалившийся образ остался бы работать).
- **`ota mark` / `ota rollback` после `ota push ... noreboot` или `ota boot`.** IDF помечает не
  работающий образ, а запись otadata, выбранную для следующей загрузки. Поэтому, пока выбран
  другой слот, обе команды отвечают `ERR ESP_ERR_INVALID_STATE` («ota_1 is selected for the next
  boot»), а `ota info` показывает `rollback_possible=false`: сначала перезагрузиться (или
  `ota boot <текущий слот>`). Иначе `ota mark valid` подтвердил бы ещё не запускавшийся образ без
  самотеста. `ota mark valid` на прошитом по USB образе заодно создаёт его запись в otadata, а
  `ota boot` на другой слот сначала записывает текущий как VALID (есть куда откатиться).
- **Батарея и защёлка питания (GPIO7).** Приложение держит GPIO7 = 1 с удержанием пада
  (`gpio_hold_en`), поэтому `reboot`, перезапуск после OTA, откат и panic не обесточивают плату на
  батарее. Удержание снимает только сброс по питанию или `board_power_off()`. Загрузчик (хук
  `power_latch`) дополнительно ставит GPIO7 = 1 сразу после ROM. Проверка: `python tools\dev.py
  reboot after=15`, за 15 с отключить USB — плата должна перезагрузиться и снова зажечь экран от батареи.
- **`reboot dl`** записывает `RTC_CNTL_FORCE_DOWNLOAD_BOOT` и делает программный сброс: ROM
  входит в режим загрузки без кнопок.
- **Никогда не прожигать eFuse**: ни шифрования flash, ни secure boot, ни HMAC-шифрования NVS.
  Тогда любое состояние выше восстановимо по USB.
- **Factory-образ** заменяет таблицу разделов: после него NVS проекта стёрта. Вернуться — полный
  `.\tools\flash.ps1`, затем `python tools\secrets_push.py secrets.json` (ключи, `ai_url` релея и
  Wi-Fi; значения не печатаются, файл в `.gitignore`).
- Загрузчик обновляется **только** по USB (полный flash); OTA меняет только слоты приложения.

## Проверки после любого восстановления

```powershell
python tools\dev.py wait-ready 15
python tools\dev.py info        # ver, part, ota_state, selftest, panel_id, gpio7=1
python tools\dev.py ota info    # running/boot/next, ok_ver, rb_reason
```

## Тесты без платы

```powershell
.\tools\py.ps1 tools\test_dev.py                                    # dev.py против эмулятора прошивки
powershell -NoProfile -ExecutionPolicy Bypass -File components\devconsole\host_test\build.ps1
components\devconsole\host_test\out\devconsole_test.exe             # транспорт консоли (MSVC), "0 failures"
```
