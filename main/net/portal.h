#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// SoftAP "RoundAI-XXXX" (random 8-digit WPA2 password per session) with a captive DNS and a
// web form at http://192.168.4.1/ for Wi-Fi and the AI key. Wi-Fi must already be initialised.
esp_err_t portal_start(void);
esp_err_t portal_stop(void);
bool portal_active(void);
// For the on-screen QR code: WIFI:T:WPA;S:<ssid>;P:<pass>;;
void portal_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
