#include "httpx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "httpx";

esp_http_client_handle_t httpx_client(const char *url, esp_http_client_method_t method, const char *api_key,
                                      int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "RoundAssistant/1.0 (ESP32-S3)",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return NULL;
    }
    if (api_key && api_key[0]) {
        char auth[230];
        snprintf(auth, sizeof(auth), "Bearer %s", api_key);
        esp_http_client_set_header(h, "Authorization", auth);
    }
    esp_http_client_set_header(h, "HTTP-Referer", "https://github.com/round-assistant");
    esp_http_client_set_header(h, "X-OpenRouter-Title", "Round Assistant");
    return h;
}

esp_err_t httpx_probe(const char *url, const char *api_key, int *status, size_t *bytes, int *ms, char *snip,
                      size_t snip_len)
{
    if (snip && snip_len) {
        snip[0] = 0;
    }
    int64_t t0 = esp_timer_get_time();
    *status = 0;
    *bytes = 0;
    esp_http_client_handle_t h = httpx_client(url, HTTP_METHOD_GET, api_key, 15000);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(h, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        char *buf = malloc(2048);
        int r;
        while (buf && (r = esp_http_client_read(h, buf, 2048)) > 0) {
            if (snip && *bytes < snip_len - 1) {
                size_t n = snip_len - 1 - *bytes < (size_t)r ? snip_len - 1 - *bytes : (size_t)r;
                memcpy(snip + *bytes, buf, n);
                snip[*bytes + n] = 0;
            }
            *bytes += r;
        }
        free(buf);
    } else {
        ESP_LOGW(TAG, "GET %s: %s", url, esp_err_to_name(err));
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    return err;
}
