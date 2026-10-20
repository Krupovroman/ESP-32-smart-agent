# ESP-32-smart-agent

Голосовой ИИ-ассистент и переводчик на круглом экране — прошивка для
**Waveshare ESP32-S3-Touch-LCD-1.85** (ESP32-S3R8, 360×360 ST77916, тач CST816,
микрофон ICS-43434, ЦАП PCM5101, Li-ion).

- **Ассистент:** нажал (сфера на экране или кнопка BOOT) — говоришь — ответ в чате и голосом.
- **Переводчик:** двусторонний перевод речи для выбранной пары языков, включая китайский/японский/корейский.
- Модели через OpenRouter (по умолчанию Gemini Flash Lite + MAI Voice), веб-поиск для свежих данных.
- Круглый интерфейс на LVGL 9: сфера с анимациями, чат, круглая клавиатура, экран блокировки, индикатор батареи.
- OTA с двумя слотами и автоматическим откатом, dev-консоль по USB (`tools/dev.py`), on-device smoke-тест.

## Сборка

ESP-IDF v6.1:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM7 flash
```

Ключ OpenRouter и Wi-Fi вводятся на устройстве или из локального `secrets.json`
(не хранится в git): `python tools/secrets_push.py secrets.json --port COM7`.
Для регионов, где OpenRouter недоступен, есть описание ретранслятора — `docs/proxy.md`.

## Документация

- `docs/STATUS.md` — текущее состояние
- `docs/architecture.md`, `docs/protocol.md`, `docs/ui.md` — устройство прошивки
- `docs/hardware.md` — плата и выводы
- `docs/rollback.md` — откат версий

## Лицензия

Код — Apache 2.0 (`LICENSE`). Шрифты — под своими лицензиями (`assets/fonts/*-LICENSE.txt`, `*-OFL.txt`).
