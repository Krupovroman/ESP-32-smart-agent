#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *ru, *en;
    uint8_t slot;
} str_def_t;

#define E(id, ru, en, slot) [id] = { ru, en, slot }

static const str_def_t s_tab[S__N] = {
    E(S_MODE_ASSISTANT, "Ассистент", "Assistant", SLOT_PILL),
    E(S_MODE_TRANSLATOR, "Переводчик", "Translator", SLOT_NONE),     // list row title (M), not the pill

    E(S_ST_LISTENING, "Слушаю" TXT_ELLIPSIS, "Listening" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_LISTENING_LANG, "Слушаю" TXT_ELLIPSIS " (%s)", "Listening" TXT_ELLIPSIS " (%s)", SLOT_STATUS),
    E(S_ST_LATCHED, "Нажмите " TXT_EMDASH " отправлю", "Tap to send", SLOT_STATUS),
    E(S_ST_CANCEL, "Отпустите " TXT_EMDASH " отмена", "Release to cancel", SLOT_STATUS),
    E(S_ST_LEFT_S, "Ещё %d с", "%d s left", SLOT_STATUS),
    E(S_ST_UPLOAD, "Отправляю %d %%", "Sending %d %%", SLOT_STATUS),
    E(S_ST_THINK, "Думаю" TXT_ELLIPSIS, "Thinking" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_THINK2, "Ещё немного" TXT_ELLIPSIS, "Almost there" TXT_ELLIPSIS, SLOT_STATUS),
    // ui.md "Долго… PWR — отмена" is 186 px in ui_16, over the 180 px conversation status slot
    E(S_ST_THINK3, "Долго" TXT_ELLIPSIS " PWR: отмена", "Slow" TXT_ELLIPSIS " PWR cancels", SLOT_STATUS),
    E(S_ST_SPEAK_HINT, "Нажмите " TXT_EMDASH " стоп", "Tap to stop", SLOT_STATUS),
    E(S_ST_NOT_HEARD, "Не расслышал", "Didn't catch that", SLOT_STATUS),
    E(S_ST_TOO_SHORT, "Слишком коротко", "Too short", SLOT_STATUS),
    E(S_ST_CANCELLED, "Отменено", "Cancelled", SLOT_STATUS),
    E(S_ST_RETRY, "Повтор %d/2" TXT_ELLIPSIS, "Retry %d/2" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_BUSY_HINT, "PWR " TXT_EMDASH " отмена", "PWR cancels", SLOT_STATUS),

    // "Держите шар" is 108 px: line 1 (between the side buttons) holds <= 100, line 2 (below them) <= 124
    E(S_HINT1, "Держите", "Hold the orb", SLOT_HINT),
    E(S_HINT2, "шар или BOOT", "or BOOT", SLOT_HINT2),
    E(S_HINT1_TR, "Говорите", "Speak", SLOT_HINT),
    E(S_HINT2_TR, "%s или %s", "%s or %s", SLOT_HINT2),

    E(S_TITLE_HISTORY, "История", "History", SLOT_TITLE),
    E(S_NEW_CHAT, "Новый разговор", "New chat", SLOT_BTN_WIDE),
    E(S_DIVIDER, TXT_EMDASH " новый разговор " TXT_MIDDOT " %s " TXT_EMDASH,
      TXT_EMDASH " new conversation " TXT_MIDDOT " %s " TXT_EMDASH, SLOT_NONE),
    E(S_TTS_NOTE, "голос недоступен", "voice unavailable", SLOT_NONE),

    E(S_VOICE_OFF, "Голос выкл.", "Voice off", SLOT_TOAST),
    E(S_VOICE_ON, "Голос вкл.", "Voice on", SLOT_TOAST),
    E(S_FINISH_INPUT, "Сначала завершите ввод", "Finish typing first", SLOT_TOAST_WIDE),   // shown in K
    E(S_USB_POWER, "Питание от USB", "Powered by USB", SLOT_TOAST),
    E(S_DONE, "Готово", "Done", SLOT_TOAST),
    E(S_NOT_YET, "Пока недоступно", "Not available yet", SLOT_TOAST),

    E(S_TOUCH_FMT, "x %d " TXT_MIDDOT " y %d " TXT_MIDDOT " %d Гц", "x %d " TXT_MIDDOT " y %d " TXT_MIDDOT " %d Hz",
      SLOT_TOAST),
    E(S_TOUCH_HELP, "Коснитесь экрана", "Touch the screen", SLOT_TOAST),

    // fault texts use ui_16 in the <= 160 px pill: "Ключ не принят ›" is 171 px in ui_20;
    // "Модель недоступна ›" / "Модель скоро отключат ›" of ui.md 5.6 do not fit even in ui_16
    E(S_F_NO_WIFI, "Нет Wi-Fi", "No Wi-Fi", SLOT_FAULT),
    E(S_F_BAD_KEY, "Ключ не принят", "Key rejected", SLOT_FAULT),
    E(S_F_NO_CREDIT, "Нет средств", "No credit", SLOT_FAULT),
    E(S_F_LOW_CREDIT, "Мало средств", "Low credit", SLOT_FAULT),
    E(S_F_REGION, "Недоступно", "Unavailable", SLOT_FAULT),
    E(S_F_MODEL, "Нет модели", "No model", SLOT_FAULT),
    E(S_F_MODEL_EXP, "Модель истекает", "Model expiring", SLOT_FAULT),
    E(S_F_MIC, "Микрофон", "Microphone", SLOT_FAULT),
    E(S_F_NO_KEY, "Нет ключа", "No key", SLOT_FAULT),       // also the card's short status text

    E(S_C_WIFI_T, "Нет подключения к Wi-Fi", "Not connected to Wi-Fi", SLOT_NONE),
    E(S_C_WIFI_B, "Подключаюсь снова" TXT_ELLIPSIS, "Reconnecting" TXT_ELLIPSIS, SLOT_NONE),
    E(S_C_WIFI_A, "Настроить", "Set up", SLOT_BTN),
    E(S_C_KEY_T, "Ключ API не подошёл (401)", "API key rejected (401)", SLOT_NONE),
    E(S_C_KEY_A, "Заменить ключ", "Replace key", SLOT_BTN),
    E(S_C_CREDIT_T, "Закончились средства", "Out of credit", SLOT_NONE),
    E(S_C_CREDIT_B, "Пополните баланс на openrouter.ai", "Top up at openrouter.ai", SLOT_NONE),
    E(S_C_CREDIT_A, "Подробнее", "Details", SLOT_BTN),
    E(S_C_REGION_T, "Сервис недоступен", "Not available", SLOT_NONE),
    E(S_C_REGION_B, "для этого региона или аккаунта. Укажите другого провайдера",
      "in your region or for this account. Choose another provider", SLOT_NONE),
    E(S_C_REGION_A, "Провайдер", "Provider", SLOT_BTN),
    E(S_C_MODEL_T, "Модель недоступна", "Model unavailable", SLOT_NONE),
    E(S_C_MODEL_A, "Выбрать", "Choose", SLOT_BTN),
    E(S_C_RATE_T, "Слишком часто " TXT_EMDASH " подождите", "Too many requests " TXT_EMDASH " wait", SLOT_NONE),
    E(S_C_SERVER_T, "Сервер не ответил", "Server didn't respond", SLOT_NONE),
    E(S_C_RETRY_A, "Повторить", "Retry", SLOT_BTN),
    E(S_C_TLS_T, "Не удаётся связаться с сервером", "Can't reach the server", SLOT_NONE),
    E(S_C_DIAG_A, "Диагностика", "Diagnostics", SLOT_BTN),
    E(S_C_MIC_T, "Микрофон молчит", "Microphone is silent", SLOT_NONE),
    E(S_C_SPK_T, "Динамик не подключён", "No speaker connected", SLOT_NONE),
    E(S_C_SPK_B, "Отвечаю текстом", "Replying in text", SLOT_NONE),
    E(S_C_SPK_A, "Проверить звук", "Test sound", SLOT_BTN),
    E(S_C_NOKEY_T, "Нет ключа API", "No API key", SLOT_NONE),
    E(S_C_NOKEY_A, "Ввести ключ", "Enter key", SLOT_BTN),
    E(S_C_ERROR_T, "Ошибка", "Error", SLOT_NONE),

    // short error for the coral status line while the orb shows ERROR (card titles wrap, these must not)
    E(S_E_GENERIC, "Ошибка", "Error", SLOT_STATUS),
    E(S_E_SERVER, "Сервер не ответил", "No server reply", SLOT_STATUS),
    E(S_E_RATE, "Слишком часто", "Too many requests", SLOT_STATUS),
    E(S_E_TLS, "Нет связи с сервером", "Server unreachable", SLOT_STATUS),
    E(S_E_MIC, "Микрофон молчит", "Mic is silent", SLOT_STATUS),
    E(S_E_SPK, "Нет динамика", "No speaker", SLOT_STATUS),

    // ---- second half
    E(S_ON, "Вкл", "On", SLOT_TOGGLE),
    E(S_OFF, "Выкл", "Off", SLOT_TOGGLE),
    E(S_NOT_SAVED, "Не сохранено", "Not saved", SLOT_TOAST),
    E(S_CANCEL, "Отмена", "Cancel", SLOT_DLG_BTN),
    E(S_LOADING, "Загрузка" TXT_ELLIPSIS, "Loading" TXT_ELLIPSIS, SLOT_ROW_SUB),
    E(S_ERR_LOAD, "Не загрузилось", "Couldn't load", SLOT_ROW),
    E(S_AUTO, "Авто", "Auto", SLOT_ROW),
    E(S_NEVER, "Никогда", "Never", SLOT_ROW),
    E(S_SEC_FMT, "%d с", "%d s", SLOT_ROW),
    E(S_MIN_FMT, "%d мин", "%d min", SLOT_ROW),

    E(S_T_MODE, "Режим", "Mode", SLOT_TITLE),
    E(S_T_SETTINGS, "Настройки", "Settings", SLOT_TITLE),
    E(S_T_WIFI, "Wi-Fi", "Wi-Fi", SLOT_TITLE),
    E(S_T_PROVIDER, "Провайдер", "Provider", SLOT_TITLE),
    E(S_T_MODELS, "Модель ИИ", "AI model", SLOT_TITLE),
    E(S_T_KEY, "Ключ API", "API key", SLOT_TITLE),
    E(S_T_VOICE, "Голос", "Voice", SLOT_TITLE),
    E(S_T_TTS, "Озвучка", "Speech model", SLOT_TITLE),
    E(S_T_VOICES, "Голоса", "Voices", SLOT_TITLE),
    E(S_T_SPEED, "Скорость", "Speed", SLOT_TITLE),
    E(S_T_LANGS, "Языки", "Languages", SLOT_TITLE),
    E(S_T_LANG_A, "Мой язык", "My language", SLOT_TITLE),
    E(S_T_LANG_B, "Собеседник", "Other person", SLOT_TITLE),
    E(S_T_DIR, "Направление", "Direction", SLOT_TITLE),
    E(S_T_DISPLAY, "Экран и сон", "Screen & sleep", SLOT_TITLE),
    E(S_T_ABOUT, "О системе", "About", SLOT_TITLE),
    E(S_T_LICENSES, "Лицензии", "Licences", SLOT_TITLE),
    E(S_T_PHONE, "Телефон", "Phone", SLOT_QR_TITLE),

    E(S_M_ASSISTANT_SUB, "Отвечает на вопросы", "Answers questions", SLOT_ROW_SUB),
    E(S_M_TYPE, "Задать вопрос", "Type a question", SLOT_ROW),     // "Написать текстом" is 182 px
    E(S_M_NEW_SUB, "Забыть контекст", "Forget the context", SLOT_ROW_SUB),
    E(S_VOLUME, "Громкость", "Volume", SLOT_ROW),
    E(S_BRIGHT, "Яркость", "Brightness", SLOT_ROW),
    E(S_VOICE_REPLY, "Озвучка", "Voice", SLOT_ROW),
    E(S_VOICE_TR, "Перевод вслух", "Speak aloud", SLOT_ROW),

    E(S_S_NOT_CONNECTED, "Не подключено", "Not connected", SLOT_ROW_SUB),
    E(S_S_CONNECTING, "Подключаюсь" TXT_ELLIPSIS, "Connecting" TXT_ELLIPSIS, SLOT_QR1),
    E(S_S_PHONE_SUB, "Настройка по QR", "Set up with a QR code", SLOT_ROW_SUB),
    E(S_S_SPEAKER, "Динамик", "Speaker", SLOT_ROW),
    E(S_S_SPK_YES, "Подключён", "Connected", SLOT_ROW_SUB),
    E(S_S_SPK_NO, "Нет", "None", SLOT_ROW_SUB),
    E(S_S_DIM_FMT, "Затемнение %s", "Dims after %s", SLOT_ROW_SUB),     // %s = "5 мин"
    E(S_S_DIM_NEVER, "Не затемнять", "Never dim", SLOT_ROW_SUB),
    E(S_S_FACE, "Лицо шара", "Orb face", SLOT_ROW),
    E(S_S_UILANG, "Язык меню", "Menu language", SLOT_ROW),
    E(S_LANG_SELF, "Русский", "English", SLOT_ROW_SUB),
    E(S_S_CLEAR, "Очистить", "Clear", SLOT_ROW_SUB),

    E(S_W_RESCAN, "Искать снова", "Scan again", SLOT_STEP_B),
    E(S_W_SCANNING, "Ищу сети" TXT_ELLIPSIS, "Scanning" TXT_ELLIPSIS, SLOT_ROW_SUB),
    E(S_W_CONNECTED, "Подключено", "Connected", SLOT_ROW_SUB),
    E(S_W_SECURED, "Защищена", "Secured", SLOT_ROW_SUB),
    E(S_W_OPEN, "Открытая", "Open", SLOT_ROW_SUB),
    E(S_W_SAVED, "Сохранена", "Saved", SLOT_ROW_SUB),
    E(S_W_AWAY, "Сохранена, не видна", "Saved, out of range", SLOT_ROW_SUB),
    E(S_W_OTHER, "Другая сеть" TXT_ELLIPSIS, "Other network" TXT_ELLIPSIS, SLOT_ROW),
    E(S_W_PHONE, "С телефона (QR)", "From phone (QR)", SLOT_STEP_P),
    E(S_W_NONE, "Нет сетей", "No networks", SLOT_ROW),
    E(S_N_CONNECT, "Подключить", "Connect", SLOT_ROW),
    E(S_N_PASSWORD, "Ввести пароль", "Enter password", SLOT_ROW),
    E(S_N_FORGET, "Забыть сеть", "Forget network", SLOT_ROW),
    E(S_N_STATUS, "Состояние", "Status", SLOT_ROW_SUB),
    E(S_N_IP, "IP-адрес", "IP address", SLOT_ROW_SUB),
    E(S_N_SIGNAL, "Сигнал", "Signal", SLOT_ROW_SUB),
    E(S_N_FORGET_Q, "Забыть сеть?", "Forget network?", SLOT_DLG_T),
    E(S_N_FORGET_BTN, "Забыть", "Forget", SLOT_DLG_BTN),
    E(S_N_FORGOTTEN, "Сеть забыта", "Network forgotten", SLOT_TOAST),
    E(S_K_PASS_FMT, "Пароль " TXT_MIDDOT " %s", "Password " TXT_MIDDOT " %s", SLOT_PROMPT),
    E(S_K_SSID, "Имя сети", "Network name", SLOT_PROMPT),
    E(S_WC_TITLE, "Подключаюсь", "Connecting", SLOT_STEP_T),
    E(S_WC_SUB_FMT, "к " TXT_LAQUO "%s" TXT_RAQUO, "to " TXT_LAQUO "%s" TXT_RAQUO, SLOT_STEP_S),
    E(S_WC_OK, "Подключено", "Connected", SLOT_STEP_T),
    E(S_WC_BAD_PASS, "Неверный пароль", "Wrong password", SLOT_STEP_T),
    E(S_WC_NOT_FOUND, "Сеть не найдена", "Network not found", SLOT_STEP_T),
    E(S_WC_FAILED, "Не подключилось", "Couldn't connect", SLOT_STEP_T),
    E(S_WC_AGAIN, "Ввести снова", "Enter again", SLOT_STEP_P),
    E(S_WC_OTHER, "Другая сеть", "Other network", SLOT_STEP_B),

    E(S_P_URL, "Адрес", "Address", SLOT_ROW),
    E(S_P_KEY_NONE, "Не задан", "Not set", SLOT_ROW_SUB),
    E(S_P_CHECK, "Проверить ключ", "Check the key", SLOT_ROW),
    E(S_P_PHONE, "С телефона", "From phone", SLOT_ROW),
    E(S_K_URL, "Адрес API", "API address", SLOT_PROMPT),
    E(S_K_KEY_FMT, "Ключ " TXT_MIDDOT " %d/64", "Key " TXT_MIDDOT " %d/64", SLOT_PROMPT),
    E(S_K_KEY, "Ключ", "Key", SLOT_PROMPT),
    E(S_K_MODEL, "Модель", "Model", SLOT_PROMPT),
    E(S_P_NEED_HTTPS, "Нужен адрес https://", "Needs https://", SLOT_TOAST),
    E(S_AI_LOAD, "Все модели", "All models", SLOT_ROW),
    E(S_AI_CUSTOM, "Свой ID" TXT_ELLIPSIS, "Custom ID" TXT_ELLIPSIS, SLOT_ROW),
    E(S_AI_PRICE_FMT, TXT_APPROX " $%s / ответ", TXT_APPROX " $%s / reply", SLOT_ROW_SUB),
    E(S_AI_DEFAULT, "по умолчанию", "default", SLOT_ROW_SUB),
    E(S_AI_EXPIRES_FMT, "истекает %s", "expires %s", SLOT_ROW_SUB),
    E(S_KI_REMAINING, "Осталось", "Remaining", SLOT_ROW_SUB),
    E(S_KI_OF_FMT, "$%s из $%s", "$%s of $%s", SLOT_ROW),
    E(S_KI_NO_LIMIT, "Лимит не задан", "No limit set", SLOT_ROW),
    E(S_KI_RESET, "Сброс", "Resets", SLOT_ROW_SUB),
    E(S_KI_TODAY, "Сегодня", "Today", SLOT_ROW_SUB),
    E(S_KI_MONTH, "За месяц", "This month", SLOT_ROW_SUB),
    E(S_KI_TOTAL, "Всего", "Total", SLOT_ROW_SUB),
    E(S_KI_FREE, "Бесплатный аккаунт", "Free account", SLOT_ROW_SUB),
    E(S_KI_DAILY, "ежедневно", "daily", SLOT_ROW),
    E(S_KI_WEEKLY, "еженедельно", "weekly", SLOT_ROW),
    E(S_KI_MONTHLY, "ежемесячно", "monthly", SLOT_ROW),
    E(S_V_MODEL, "Модель озвучки", "Speech model", SLOT_ROW),
    E(S_V_SPEED, "Скорость речи", "Speech speed", SLOT_ROW),
    E(S_V_NO_VOICES, "Нет голосов", "No voices", SLOT_ROW),
    E(S_V_PRICE_FMT, TXT_APPROX " $%s / 1000 знаков", TXT_APPROX " $%s / 1000 chars", SLOT_ROW_SUB),

    E(S_A_FW, "Прошивка", "Firmware", SLOT_ROW_SUB),
    E(S_A_ROLLBACK, "есть откат", "can roll back", SLOT_ROW),
    E(S_A_PANEL, "Панель", "Panel", SLOT_ROW_SUB),
    E(S_A_UPTIME, "Время работы", "Uptime", SLOT_ROW_SUB),
    E(S_A_MEM, "Память: SRAM " TXT_MIDDOT " PSRAM", "Free: SRAM " TXT_MIDDOT " PSRAM", SLOT_ROW_SUB),
    E(S_A_LATENCY, "Последний ответ", "Last reply", SLOT_ROW_SUB),
    E(S_A_LAT_FMT, TXT_UP "%s с " TXT_MIDDOT " ответ %s с", TXT_UP "%s s " TXT_MIDDOT " reply %s s", SLOT_NONE),
    E(S_A_LAST_ERR, "Последняя ошибка", "Last error", SLOT_ROW_SUB),
    E(S_A_NONE, TXT_EMDASH, TXT_EMDASH, SLOT_ROW),
    E(S_A_TOUCH, "Тест касания", "Touch test", SLOT_ROW),
    E(S_A_HINTS, "Подсказки", "Hints", SLOT_ROW),
    E(S_A_HINTS_SUB, "Показать снова", "Show again", SLOT_ROW_SUB),
    E(S_A_HINTS_ON, "Подсказки включены", "Hints are back", SLOT_TOAST),
    E(S_A_REBOOT, "Перезагрузить", "Restart", SLOT_ROW),
    E(S_A_REBOOT_Q, "Перезагрузить?", "Restart now?", SLOT_DLG_T),
    E(S_A_YES, "Да", "Yes", SLOT_DLG_BTN),
    E(S_A_RESET, "Сброс настроек", "Factory reset", SLOT_ROW),
    E(S_A_RESET_Q, "Сбросить настройки?", "Reset all settings?", SLOT_DLG_T),
    E(S_A_RESET_B, "Ключ, модель и настройки будут стёрты. Устройство перезагрузится.",
      "The key, model and settings are erased. The device restarts.", SLOT_NONE),
    E(S_A_RESET_BTN, "Сбросить", "Reset", SLOT_DLG_BTN),
    E(S_H_CLEAR_Q, "Очистить историю?", "Clear the history?", SLOT_DLG_T),
    E(S_H_CLEAR_B, "Разговоры обоих режимов будут удалены.", "Conversations of both modes are deleted.",
      SLOT_NONE),
    E(S_H_DELETE, "Удалить", "Delete", SLOT_DLG_BTN),
    E(S_H_CLEARED, "История очищена", "History cleared", SLOT_TOAST),

    // with "RoundAI-7568" / "12345678": 158 / 145 px (caption 2 holds 150)
    E(S_Q_NET_FMT, "Сеть: %s", "Network: %s", SLOT_NONE),
    E(S_Q_PASS_FMT, "Пароль: %s", "Pass: %s", SLOT_NONE),
    E(S_Q_OPEN, "Откройте 192.168.4.1", "Open 192.168.4.1", SLOT_QR1),
    E(S_Q_WAIT, "Ждём данные" TXT_ELLIPSIS, "Waiting" TXT_ELLIPSIS, SLOT_QR2),
    E(S_Q_STARTING, "Запускаю сеть" TXT_ELLIPSIS, "Starting" TXT_ELLIPSIS, SLOT_QR1),
    E(S_Q_WIFI_OK, "Wi-Fi " SYM_CHECK, "Wi-Fi " SYM_CHECK, SLOT_QR_LINE),
    E(S_Q_KEY_OK, "Ключ API " SYM_CHECK, "API key " SYM_CHECK, SLOT_QR_LINE),
    E(S_Q_MODEL_OK, "Модель " SYM_CHECK, "Model " SYM_CHECK, SLOT_QR_LINE),
    E(S_Q_ERROR, "Не удалось запустить", "Couldn't start", SLOT_QR1),

    E(S_O1_T, "Привет! " TXT_MIDDOT " Hello!", "Привет! " TXT_MIDDOT " Hello!", SLOT_STEP_T),
    E(S_O1_S, "Выберите язык " TXT_MIDDOT " Choose a language", "Выберите язык " TXT_MIDDOT " Choose a language",
      SLOT_STEP_S),
    E(S_O1_P, "Русский", "Русский", SLOT_STEP_P),
    E(S_O1_B, "English", "English", SLOT_STEP_B),
    E(S_O2_T, "Подключим Wi-Fi", "Let's set up Wi-Fi", SLOT_STEP_T),
    E(S_O2_S, "Телефон настроит всё сразу", "Your phone can set up everything", SLOT_STEP_S),
    E(S_O2_B, "Выбрать сеть", "Pick a network", SLOT_STEP_B),
    E(S_O3_T, "Ключ OpenRouter", "OpenRouter key", SLOT_STEP_T),
    E(S_O3_S, "Ключ: openrouter.ai/keys", "Get one at openrouter.ai/keys", SLOT_STEP_S),
    E(S_O3_B, "Ввести вручную", "Type it here", SLOT_STEP_B),
    E(S_O4_T, "Динамик", "Speaker", SLOT_STEP_T),
    E(S_O4_S, "Подключён ли динамик?", "Is a speaker connected?", SLOT_STEP_S),
    E(S_O4_P, "Есть динамик", "Yes, it is", SLOT_STEP_P),
    E(S_O4_B, "Нет динамика", "No speaker", SLOT_STEP_B),
    E(S_O5_T, "Готово!", "All set!", SLOT_STEP_T),
    E(S_O5_S, "Держите шар и говорите", "Hold the orb and speak", SLOT_STEP_S),
    E(S_O5_P, "Начать", "Start", SLOT_STEP_P),

    E(S_K_QUESTION, "Вопрос", "Question", SLOT_PROMPT),
    E(S_K_INPUT, "Ввод", "Input", SLOT_PROMPT),
    E(S_NEW_CHAT_Q, "Начать новый разговор?", "Start a new conversation?", SLOT_DLG_T),
};

static int s_lang;

const char *ui_s(ui_sid_t id)
{
    if ((unsigned)id >= S__N || !s_tab[id].ru) {
        return "?";
    }
    return s_lang ? s_tab[id].en : s_tab[id].ru;
}

void ui_str_set_lang(int lang) { s_lang = lang ? 1 : 0; }
int ui_str_lang(void) { return s_lang; }

// ---- UTF-8 helpers (local, so the sanitiser does not depend on LVGL's private text API)

static uint32_t utf8_next(const char *s, size_t *i)
{
    const uint8_t *p = (const uint8_t *)s + *i;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else if (c >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        n = 3;
    } else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (p[1] & 0x3Fu);
        n = 2;
    } else if (c >= 0x80) {
        c = 0xFFFD;   // stray continuation byte
    }
    *i += n;
    return c;
}

static size_t utf8_put(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static bool is_emoji(uint32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0x200D || c == 0x20E3;
}

static bool font_has(const lv_font_t *font, uint32_t c)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, c, 0);
}

size_t ui_str_sanitize(const char *in, char *out, size_t out_len, const lv_font_t *font)
{
    if (!out || out_len == 0) {
        return 0;
    }
    size_t o = 0, i = 0;
    int newlines = 0;           // newlines seen in the current run
    bool line_start = true;
    while (in && in[i]) {
        uint32_t c = utf8_next(in, &i);
        if (c == 0x2028 || c == 0x2029) {
            c = '\n';
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (o == 0) {
                continue;       // no leading blank lines
            }
            if (++newlines <= 2 && o + 1 < out_len) {
                out[o++] = '\n';
            }
            line_start = true;
            continue;
        }
        if (c == '\t') {
            c = ' ';
        }
        if (line_start && c == '-' && in[i] == ' ') {
            c = 0x2022;         // "- item" -> "• item"
        }
        if (c == '#' || c == '*' || c == '_' || c == '`' || c == '>' || c < 0x20 || is_emoji(c)) {
            continue;
        }
        if (font && c != ' ' && !font_has(font, c)) {
            continue;
        }
        if (c == ' ' && (line_start || (o > 0 && out[o - 1] == ' '))) {
            continue;           // indentation and double spaces left by stripped markdown / emoji
        }
        char enc[4];
        size_t n = utf8_put(c, enc);
        if (o + n >= out_len) {
            break;
        }
        memcpy(out + o, enc, n);
        o += n;
        newlines = 0;
        line_start = false;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) {
        o--;
    }
    out[o] = 0;
    return o;
}

bool ui_str_font_has_all(const char *s, const lv_font_t *font)
{
    size_t i = 0;
    while (s && s[i]) {
        uint32_t c = utf8_next(s, &i);
        if (c != '\n' && c != ' ' && !font_has(font, c)) {
            return false;
        }
    }
    return true;
}

size_t ui_str_cp_count(const char *s)
{
    size_t n = 0;
    for (; s && *s; s++) {
        n += ((uint8_t)*s & 0xC0) != 0x80;
    }
    return n;
}

size_t ui_str_copy(char *dst, size_t len, const char *src)
{
    if (!dst || len == 0) {
        return 0;
    }
    size_t n = src ? strlen(src) : 0;
    if (n >= len) {
        // truncated: back off over continuation bytes so a 2-4 byte sequence is never split
        n = len - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = 0;
    return n;
}

// ---- Fit test (ui.md 7.9): every string measured against its slot, both languages

typedef struct {
    const lv_font_t *font;
    int32_t max_w;
    const char *suffix;
} slot_def_t;

static slot_def_t slot_of(uint8_t slot)
{
    switch (slot) {
    case SLOT_PILL: return (slot_def_t){ &ui_20, 132, " " SYM_DOWN };   // 140 px pill, 4 px each side
    case SLOT_FAULT: return (slot_def_t){ &ui_16, 152, " " TXT_RSAQUO };  // fault pill (<= 160 wide), ui_16
    case SLOT_STATUS: return (slot_def_t){ &ui_16, 180, "" };           // conversation width (empty is 200)
    case SLOT_TITLE: return (slot_def_t){ &ui_20, 147, "" };            // arc title after the chevron
    case SLOT_HINT: return (slot_def_t){ &ui_16, 100, "" };             // H hint line 1, between the buttons
    case SLOT_HINT2: return (slot_def_t){ &ui_16, 124, "" };            // H hint line 2, below the buttons
    case SLOT_TOAST: return (slot_def_t){ &ui_16, 180, "" };            // Home status slot (conversation)
    case SLOT_TOAST_WIDE: return (slot_def_t){ &ui_16, 200, "" };       // top-layer toast on other screens
    case SLOT_BTN: return (slot_def_t){ &ui_20, 184, "" };              // card action stadium (<= 224 wide)
    case SLOT_BTN_WIDE: return (slot_def_t){ &ui_20, 176, "" };         // 200 px stadium
    case SLOT_ROW: return (slot_def_t){ &ui_20, 164, "" };              // list row title (3.7)
    case SLOT_ROW_SUB: return (slot_def_t){ &ui_16, 184, "" };          // row subtitle / info caption
    case SLOT_TOGGLE: return (slot_def_t){ &ui_16, 44, "" };            // right slot "Вкл" / "Выкл"
    case SLOT_PROMPT: return (slot_def_t){ &ui_16, 150, "" };           // K prompt (3.8), "%s" = "EN"
    case SLOT_STEP_T: return (slot_def_t){ &ui_28, 280, "" };           // T_STEP title (3.12)
    case SLOT_STEP_S: return (slot_def_t){ &ui_16, 290, "" };           // T_STEP subtitle
    case SLOT_STEP_P: return (slot_def_t){ &ui_20, 204, "" };           // 236 px primary stadium
    case SLOT_STEP_B: return (slot_def_t){ &ui_20, 168, "" };           // 200 px secondary stadium
    case SLOT_DLG_T: return (slot_def_t){ &ui_20, 260, "" };            // dialog title (3.11)
    case SLOT_DLG_BTN: return (slot_def_t){ &ui_20, 104, "" };          // 120 px dialog stadium
    case SLOT_QR1: return (slot_def_t){ &ui_16, 220, "" };              // Q caption 1 (3.10)
    case SLOT_QR2: return (slot_def_t){ &ui_16, 150, "" };              // Q caption 2
    case SLOT_QR_TITLE: return (slot_def_t){ &ui_20, 100, "" };         // Q arc title
    case SLOT_QR_LINE: return (slot_def_t){ &ui_20, 220, "" };          // Q "received" lines
    default: return (slot_def_t){ NULL, 0, "" };
    }
}

// Expands printf conversions with wide sample values so the measurement is a worst case.
static void expand_sample(const char *fmt, char *out, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; fmt[i] && o + 4 < len; i++) {
        if (fmt[i] == '%' && fmt[i + 1] == 'd') {
            memcpy(out + o, "100", 3);
            o += 3;
            i++;
        } else if (fmt[i] == '%' && fmt[i + 1] == 's') {
            memcpy(out + o, "EN", 2);   // %s is a language code everywhere
            o += 2;
            i++;
        } else if (fmt[i] == '%' && fmt[i + 1] == '%') {
            out[o++] = '%';
            i++;
        } else {
            out[o++] = fmt[i];
        }
    }
    out[o] = 0;
}

int ui_str_fit_check(void (*out)(const char *line))
{
    int bad = 0;
    char sample[160], line[224];
    for (int id = 0; id < S__N; id++) {
        slot_def_t sd = slot_of(s_tab[id].slot);
        if (!sd.font) {
            continue;
        }
        for (int lang = 0; lang < 2; lang++) {
            const char *txt = lang ? s_tab[id].en : s_tab[id].ru;
            char fmt[160];
            snprintf(fmt, sizeof(fmt), "%s%s", txt, sd.suffix);
            expand_sample(fmt, sample, sizeof(sample));
            lv_point_t sz;
            lv_text_get_size(&sz, sample, sd.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x > sd.max_w) {
                bad++;
                if (out) {
                    snprintf(line, sizeof(line), "overflow id=%d %s w=%d max=%d \"%s\"", id, lang ? "en" : "ru",
                             (int)sz.x, (int)sd.max_w, sample);
                    out(line);
                }
            }
        }
    }
    return bad;
}
