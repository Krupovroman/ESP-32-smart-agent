#pragma once
#include "esp_err.h"

// REPL on the USB-Serial-JTAG port (prompt "ra>"). Every command ends with a line
// "OK" or "ERR <reason>" so tools/dev.py can script it. Commands: help, info, heap,
// shot, tap, press, move, release, swipe, bl, invert, touchlog, mic, spk, reboot.
esp_err_t devconsole_start(void);
