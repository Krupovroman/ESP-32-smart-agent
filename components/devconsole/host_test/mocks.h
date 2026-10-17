#pragma once
// Host stand-ins for the devconsole transport test: scripted USB input, captured USB output,
// a simulated clock and a recursive-lock depth counter.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern int g_checks, g_fails;
int host_fail(const char *file, int line, const char *what);
#define CHECK(c) (g_checks++, (c) ? 1 : host_fail(__FILE__, __LINE__, #c))

extern bool g_usb_connected;
extern size_t g_tx_room;            // bytes the TX ring still takes (SIZE_MAX = unlimited)
extern int g_lock_depth;            // writer lock nesting (0 between commands)
extern uint32_t g_last_lock_wait;   // timeout (ms) of the last writer lock request
extern void *g_cur_task;            // what xTaskGetCurrentTaskHandle() returns
extern void *g_created_task;        // handle handed out by xTaskCreatePinnedToCore()
extern int64_t g_now_us;

void rx_push(const char *s);
size_t rx_pending(void);
// Everything written to the port since the last call (NUL-terminated, caller frees).
char *tx_take(void);
