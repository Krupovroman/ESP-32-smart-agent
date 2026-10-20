## Что сделано / What

<!-- Коротко: что и зачем / Briefly: what and why. Ссылка на issue / Link the issue: Closes #… -->

## Как проверено / How it was tested

- [ ] `idf.py build` без предупреждений / builds without new warnings
- [ ] На плате / on the board: `python tools/smoke.py` (или `--no-ai`)
- [ ] Host-тесты затронутых модулей / host tests of the touched modules (`*/host_test/build.ps1`)
- [ ] Скриншоты для изменений интерфейса / screenshots for UI changes (`python tools/dev.py shot …`)
- [ ] Документация обновлена, если менялось поведение / docs updated if behavior changed (`docs/`)

## Безопасность / Security

- [ ] В изменениях нет ключей, паролей Wi-Fi, адресов ретранслятора, MAC-адресов и личных данных /
      no keys, Wi-Fi passwords, relay addresses, MAC addresses or personal data in the diff
