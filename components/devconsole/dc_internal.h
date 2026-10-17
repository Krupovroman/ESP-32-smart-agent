#pragma once
// Internals shared by the devconsole sources (not for other components).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "devconsole.h"

#define DC_LINE_MAX     4200    // longest accepted input line (upload data lines)
#define DC_CMD_MAX      1024    // longest accepted command line
#define DC_BLOB_CHUNK   3072    // raw bytes per outgoing D line (4096 base64 characters)
// The USB-Serial-JTAG ISR drops received bytes when its RX ring is full, so at most
// DC_UP_WINDOW data lines may be in flight: DC_UP_WINDOW * line length < DC_USB_RX_RING.
#define DC_UP_CHUNK     1024    // raw bytes per incoming D line (1368 base64 characters)
#define DC_UP_WINDOW    2
#define DC_USB_RX_RING  4096
#define DC_USB_TX_RING  2048

// Line writer. A line is written while holding the writer lock, so it is never split by
// other output. important = command output: wait longer for a slow host before dropping.
bool dc_line_begin(bool important);
void dc_line_put(const char *s, size_t n);
void dc_line_puts(const char *s);
void dc_line_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Writes s as a JSON string literal (quotes and escapes included).
void dc_line_json_str(const char *s, size_t n);
void dc_line_end(void);

// Id of the command being executed (0 = typed by a human).
uint16_t dc_cur_id(void);
// Next input line for uploads (NUL-terminated, valid until the next call).
// Returns its length, -1 on timeout or -2 if it was longer than DC_LINE_MAX.
int dc_read_line(char **line, uint32_t timeout_ms);

// Base64 (RFC 4648, with padding). b64_decode returns the decoded length or -1.
size_t dc_b64_encode(const uint8_t *in, size_t n, char *out);
int dc_b64_decode(const char *in, size_t n, uint8_t *out, size_t out_max);
// Known-answer checks of CRC-32 and base64; logs and returns false on a mismatch.
bool dc_codec_selfcheck(void);

// Registers a command that works before devconsole_ready() (it needs no late subsystem).
void dc_register_early(const char *name, const char *help, dc_cmd_fn fn);

// Built-in command groups.
void cmd_sys_register(void);
void cmd_io_register(void);
void cmd_audio_register(void);
void cmd_ota_register(void);

// Snapshot of the boot notes for "info": heap stages and devconsole_info_set() pairs.
void dc_info_print_notes(void);
// Milliseconds since boot when devconsole_ready() ran (0 = not yet).
uint32_t dc_boot_ms(void);
const char *dc_reset_reason_name(int reason);
