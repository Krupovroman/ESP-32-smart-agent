#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Init results that the boot self-test judges (ESP_OK = the subsystem came up).
typedef struct {
    esp_err_t board, display, touch, audio_in, audio_out, wifi;
} selftest_inputs_t;

// First call in app_main after the power latch: updates the RTC crash counter. Returns true
// (safe mode: console and display only) after 3 crashes in a row that never reached READY,
// unless the image still awaits OTA confirmation (then the bootloader rollback handles it).
bool selftest_boot_begin(void);

// Boot self-test of docs/architecture.md §8.2 under a 10 s restart guard. Pass: a pending OTA
// image is confirmed. Fail: a pending image is rolled back (does not return); a USB-flashed
// image keeps running. Emits "@! EVT selftest".
esp_err_t selftest_run(const selftest_inputs_t *in);

// Announces "@! READY" (unlocking all console commands unless in safe mode) and clears the
// crash counter.
void selftest_ready(void);
