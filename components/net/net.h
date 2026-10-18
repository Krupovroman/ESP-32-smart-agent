#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    int8_t rssi;
    char ip[16];
    bool portal;
    char ap_ssid[33];
    char ap_pass[12];
    int saved;            // number of saved networks
} net_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;         // wifi_auth_mode_t
    uint8_t ch;
} net_ap_t;

#define NET_MAX_SAVED 5

// esp_netif + default event loop + Wi-Fi driver (RAM storage; networks live in NVS "wifi").
esp_err_t net_init(void);
// Connects to the strongest saved network and keeps reconnecting (1, 2, 4 … 60 s backoff).
esp_err_t net_start(void);
// Connects now; on success the network is saved (most recent first) when save is set.
esp_err_t net_connect(const char *ssid, const char *pass, bool save);
// net_connect() with the stored password of a saved network (ESP_ERR_NOT_FOUND if it is not saved).
esp_err_t net_connect_saved(const char *ssid);
esp_err_t net_disconnect(void);
esp_err_t net_forget(const char *ssid);
esp_err_t net_scan(uint32_t timeout_ms, net_ap_t *out, int max, int *n);
void net_status(net_status_t *out);
bool net_wait_up(uint32_t timeout_ms);
const char *net_state_name(net_state_t s);

// Saved network names (no passwords) for the settings UI.
int net_saved_list(char ssids[][33], int max);
