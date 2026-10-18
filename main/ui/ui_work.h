#pragma once
// Background work for the UI (docs/ui.md 7.7, architecture 1.7): Wi-Fi, HTTP, portal and NVS calls never run in
// taskLVGL. A job's fn runs in the "ui_work" task (internal-RAM stack, because flash writes from a PSRAM stack
// assert), then its done callback runs in taskLVGL through the ui_post queue. The task exists only while there
// is work: it exits after 20 s without jobs and is created again by the next ui_work().
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*ui_job_fn_t)(void *arg);

// Queue and mutex (ui_init, before any job).
void ui_work_init(void);
// Queues fn(arg) (any task); done(arg) then runs in taskLVGL (NULL = none). arg is NULL or from ui_work_alloc();
// the worker frees it after done. false when the queue is full: arg is freed and nothing runs.
bool ui_work(ui_job_fn_t fn, ui_job_fn_t done, void *arg);
// Zeroed PSRAM job argument (NULL on OOM).
void *ui_work_alloc(size_t n);
// Runs done(arg) for a finished job and frees arg (ui.c pump, UI_WORK_DONE).
void ui_work_dispatch(void *job);

// ---- Debounced settings writes (taskLVGL). The latest value per key wins; the write happens 1.5 s after the
// last change and only while the app is idle (at most 20 s later), so NVS never stalls the caches mid-turn.
void ui_save_int(const char *key, int32_t v);
void ui_save_str(const char *key, const char *v);
// Writes everything pending now (before a restart or reset).
void ui_save_flush(void);
// Setting value as the user will see it: the pending value when there is one, else settings_get_*().
int32_t ui_cfg_int(const char *key);
size_t ui_cfg_str(const char *key, char *buf, size_t len);
