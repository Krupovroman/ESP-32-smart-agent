# Безопасность / Security

## Никогда не публикуйте ключи / Never post keys

**RU.** В issue, pull request, логах и скриншотах не должно быть:

- ключей OpenRouter и других провайдеров (`sk-or-…`), даже «старых» или с лимитом;
- паролей Wi-Fi и названий домашних сетей;
- адреса и секретного пути вашего ретранслятора (`ai_url` вида `https://<хост>/<секрет>/api/v1`);
- MAC-адресов, IP-адресов и личной переписки из чата.

Если ключ всё-таки попал в публичное место — **сразу отзовите его** на [openrouter.ai/keys](https://openrouter.ai/keys)
и создайте новый: удаление комментария или коммита не помогает, копии остаются.

**EN.** Issues, pull requests, logs and screenshots must not contain:

- OpenRouter or other provider keys (`sk-or-…`), even old or capped ones;
- Wi-Fi passwords or home network names;
- the address and secret path of your relay (an `ai_url` like `https://<host>/<secret>/api/v1`);
- MAC addresses, IP addresses or personal chat content.

If a key ever leaks, **revoke it right away** at [openrouter.ai/keys](https://openrouter.ai/keys) and create a new one:
deleting the comment or the commit is not enough, copies remain.

## Как секреты держатся вне git / How secrets stay out of git

- Ключи и пароли Wi-Fi хранятся **только в NVS платы**. Они вводятся на устройстве, со страницы настройки
  с телефона или из локального `secrets.json` через `tools/secrets_push.py`.
  Keys and Wi-Fi passwords live **only in the board's NVS**. They are entered on the device, on the phone setup page,
  or pushed from a local `secrets.json` with `tools/secrets_push.py`.
- `secrets.json`, `secrets.h` и `*.secret` перечислены в [.gitignore](.gitignore); `secrets_push.py` никогда не печатает значения.
  `secrets.json`, `secrets.h` and `*.secret` are in [.gitignore](.gitignore); `secrets_push.py` never prints the values.
- Секретные настройки (`ai_key`, `tts_key`, `stt_key`) помечены в таблице настроек и не выводятся консолью целиком —
  показываются только первые и последние символы и длина.
  Secret settings (`ai_key`, `tts_key`, `stt_key`) are flagged in the settings table and never printed in full by the console;
  only the first and last few characters and the length are shown.
- Ключ уходит только по HTTPS; `esp_http_client` не пишет URL в лог.
  The key is only ever sent over HTTPS; `esp_http_client` never logs the URL.
- Сброс к заводским настройкам стирает весь раздел NVS — ключ и пароли физически удаляются.
  A factory reset erases the whole NVS partition, so the key and passwords are physically gone.
- Дампы флеша (`backups/*.bin`) и бинарники релизов в git не попадают.
  Flash dumps (`backups/*.bin`) and release binaries are never committed.

## Сообщить об уязвимости / Reporting a vulnerability

**RU.** Не описывайте уязвимость в публичном issue. Используйте закрытое сообщение GitHub:
вкладка **Security → Report a vulnerability** этого репозитория. Если она недоступна — создайте issue без
подробностей с просьбой связаться.

**EN.** Please do not describe a vulnerability in a public issue. Use GitHub's private report:
the repository's **Security → Report a vulnerability** tab. If that is not available, open an issue with no details
asking for a private contact.

Поддерживается только последняя версия (`main` и последний тег). / Only the latest version (`main` and the latest tag) is supported.
