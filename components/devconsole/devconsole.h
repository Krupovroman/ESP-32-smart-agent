#pragma once
#include "esp_err.h"

// Dev console on the USB-Serial-JTAG port (prompt "ra>"). Every command ends with a line
// "OK" or "ERR <code>" so tools/dev.py can script it. Built-in commands: help, info, heap,
// shot, tap, press, move, release, swipe, bl, invert, touchlog, mic, spk, reboot, plus
// get/set/settings/wifi/portal/net from cmd_net.c.
esp_err_t devconsole_start(void);

// A command prints its output with printf (one complete line per call) and returns 0 on
// success or non-zero on failure; the console prints the closing OK/ERR line itself.
// Never print "OK"/"ERR" from a command. May be called before or after devconsole_start().
typedef int (*dc_cmd_fn)(int argc, char **argv);
void devconsole_register(const char *name, const char *help, dc_cmd_fn fn);

// Prints "error: <message>" and returns 1, for `return dc_fail("...")`.
int dc_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Settings, Wi-Fi, portal and HTTPS commands (cmd_net.c).
void cmd_net_register(void);
