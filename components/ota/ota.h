#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

// A/B OTA, boot confirmation and rollback (docs/architecture.md §1.14, §8). Owns the NVS
// namespace "sys": boot_cnt, ok_ver (last version that passed the self-test), rb_reason, rb_cnt.

typedef enum { OTA_ST_NOT_RUN, OTA_ST_OK, OTA_ST_FAIL, OTA_ST_SKIPPED } ota_selftest_t;

typedef struct {
    char running[17], next[17], boot[17];   // partition labels ("" if none)
    esp_ota_img_states_t state;             // state of the running image
    bool state_known;                       // false for images flashed over USB (no otadata entry)
    char ver[32], next_ver[32];             // app versions in the running / next slot
    char ok_ver[32], rb_reason[64];
    uint32_t boot_cnt;
    uint16_t rb_cnt;
    bool rollback_possible;
    ota_selftest_t selftest;
    char selftest_reason[64];
    bool busy;                              // a push or URL update is running
} ota_info_t;

void ota_get_info(ota_info_t *o);
// "new", "pending_verify", "valid", "invalid", "aborted", "undefined".
const char *ota_state_name(esp_ota_img_states_t s);
const char *ota_selftest_name(ota_selftest_t s);
// True when the running image was written through esp_ota_* and still awaits confirmation.
bool ota_pending_verify(void);

// Self-test helper: sys/boot_cnt += 1, written and read back. Returns ESP_ERR_INVALID_CRC on a
// read-back mismatch.
esp_err_t ota_boot_count_bump(uint32_t *out);
// Records the self-test verdict. ok: confirms a pending image and stores sys/ok_ver.
// !ok: stores sys/rb_reason; a pending image is marked invalid and the chip reboots into the
// previous slot (does not return); a USB-flashed image keeps running for debugging.
esp_err_t ota_confirm_boot(bool ok, const char *reason);
// Marks the self-test as not run (safe mode).
void ota_selftest_skipped(const char *reason);

// Console push: esp_ota_begin(next slot, OTA_WITH_SEQUENTIAL_WRITES), so flash is erased
// sector by sector as data arrives.
esp_err_t ota_stream_begin(size_t size);
esp_err_t ota_stream_write(const void *p, size_t n);
// Validates the image (esp_ota_end); set_boot selects it for the next boot (no restart).
esp_err_t ota_stream_end(bool set_boot);
void ota_stream_abort(void);

// Progress of ota_start_https: pct 0..100 while running; done = true once at the end with the
// result (on success the chip restarts about 1 s later).
typedef void (*ota_progress_cb_t)(int pct, size_t bytes, size_t total, bool done, esp_err_t err);
void ota_set_progress_cb(ota_progress_cb_t cb);
// Downloads url (https, CA bundle) into the next slot in task "ota", then boots it.
esp_err_t ota_start_https(const char *url);

// Selects "ota_0"/"ota_1" for the next boot (no restart; the caller restarts). Switching away
// from a USB-flashed image first records it in otadata, so the other slot can roll back to it.
esp_err_t ota_set_boot(const char *label);
// Marks the running image invalid and reboots into the previous one, if that one is valid.
esp_err_t ota_rollback_now(void);
// valid: esp_ota_mark_app_valid_cancel_rollback() (a USB-flashed image gets its otadata entry);
// !valid: mark invalid, rolls back on the next boot. Both refuse with ESP_ERR_INVALID_STATE
// while another slot is selected for the next boot (IDF would mark that slot instead).
esp_err_t ota_mark(bool valid);
