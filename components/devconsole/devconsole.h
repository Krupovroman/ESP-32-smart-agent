#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Dev console, protocol v2 (docs/architecture.md §7) on the USB-Serial-JTAG port.
//   PC  -> "[#<id> ]<command> [args]"          (no id = id 0, a human in idf.py monitor)
//   DEV -> "@<id> OK <json>" | "@<id> ERR <esp_err_name> {"msg":...}"
//          "@<id> BEGIN|D|END ..." blobs, "@<id> READY|ACK ..." uploads,
//          "@! EVT {...}" events, "@! READY {...}" once booted; any other line is a log line.
// All output (printf, ESP_LOG, events) goes through one writer, so lines never interleave,
// and writes are dropped instead of blocking when no host reads the port.
esp_err_t devconsole_start(void);

// A command prints its output with printf (one complete line per call) and returns 0 on
// success or non-zero on failure; the console prints the closing OK/ERR line itself.
// Never print "OK"/"ERR" from a command. May be called before or after devconsole_start().
// The printed lines become {"out":[...]} of the OK line; a printed line that is a JSON object
// ("{...}") becomes the OK payload itself (other lines are added to it as "out").
// A non-zero return that is an esp_err_t is reported by name, anything else as ESP_FAIL.
typedef int (*dc_cmd_fn)(int argc, char **argv);
void devconsole_register(const char *name, const char *help, dc_cmd_fn fn);

// Records "<message>" as the ERR line's "msg" (a human at id 0 also sees "error: <message>")
// and returns 1, for `return dc_fail("...")`.
int dc_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Settings, Wi-Fi, portal and HTTPS commands (cmd_net.c).
void cmd_net_register(void);

// ---- Helpers for commands (call only from inside a command handler) ----

// Value of a "key=value" argument (argv[1..]), or NULL.
const char *dc_arg(int argc, char **argv, const char *key);

// Sends a binary payload as "@id BEGIN <kind> {meta,"bytes","crc32"}", "@id D <seq> <base64>"
// lines (3072 bytes each) and "@id END {...}". meta is a JSON member list without braces
// (e.g. "\"w\":360,\"h\":360") or NULL. Returns 0, or 1 after dc_fail().
// (Lines a command prints in the v1 format "BLOB:BEGIN <kind> <bytes> <crc32> [args]",
// "BLOB:<base64>", "BLOB:END" are converted to these frames as well.)
int dc_blob(const char *kind, const void *data, size_t len, const char *meta);

// Receives an upload from the PC (tools/dev.py Device.upload): prints "@id READY {"chunk","window"}",
// takes "D <seq> <base64>" lines (ACK after every window), hands each decoded chunk to sink and
// checks size and CRC-32 at "END". "ABORT", any other line or 5 s of silence abort the upload.
typedef esp_err_t (*dc_sink_fn)(void *ctx, const uint8_t *data, size_t len, size_t offset);
esp_err_t dc_upload(size_t bytes, uint32_t crc32, dc_sink_fn sink, void *ctx);

// Runs fn(arg) after the command's OK/ERR line has been sent (reboot, rollback, slot switch).
void dc_defer(void (*fn)(void *arg), void *arg);

// ---- Events and boot status (any task) ----

// "@! EVT {"t":<ms>,"ev":"<ev>"[,<members>]}"; fmt (may be NULL) formats further JSON members,
// e.g. devcon_event("ota", "\"pct\":%d", pct).
void devcon_event(const char *ev, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Prints "@! READY {...}" (version, reset reason, boot count, slot, boot_ms). With unlock the
// commands that need initialised subsystems are enabled; until then (and in safe mode, where
// unlock is false) they answer ERR ESP_ERR_INVALID_STATE "not ready".
void devconsole_ready(bool unlock);
bool devconsole_is_ready(void);
// True while the console task is alive (boot self-test).
bool devconsole_running(void);

// Records the free internal heap now under name (shown by "info" as heap_stages).
void devconsole_heap_stage(const char *name);
// Adds "key": <json_value> to the "info" output (e.g. "safe_mode", "true"); the value is copied.
void devconsole_info_set(const char *key, const char *json_value);
