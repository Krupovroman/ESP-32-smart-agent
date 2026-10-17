#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct mock_client *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET = 0, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_HEAD } esp_http_client_method_t;
typedef enum {
    HTTP_EVENT_ERROR = 0, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_HEADERS_COMPLETE, HTTP_EVENT_ON_STATUS_CODE, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT
} esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    bool disable_auto_redirect;
    int max_authorization_retries;
    http_event_handle_cb event_handler;
    int buffer_size;
    int buffer_size_tx;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
    const char *user_agent;
} esp_http_client_config_t;

#define ESP_ERR_HTTP_BASE (0x7000)
#define ESP_ERR_HTTP_MAX_REDIRECT (ESP_ERR_HTTP_BASE + 1)
#define ESP_ERR_HTTP_CONNECT (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_WRITE_DATA (ESP_ERR_HTTP_BASE + 3)
#define ESP_ERR_HTTP_FETCH_HEADER (ESP_ERR_HTTP_BASE + 4)
#define ESP_ERR_HTTP_INVALID_TRANSPORT (ESP_ERR_HTTP_BASE + 5)
#define ESP_ERR_HTTP_CONNECTING (ESP_ERR_HTTP_BASE + 6)
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)
#define ESP_ERR_HTTP_CONNECTION_CLOSED (ESP_ERR_HTTP_BASE + 8)
#define ESP_ERR_HTTP_READ_TIMEOUT (ESP_ERR_HTTP_BASE + 11)
#define ESP_ERR_HTTP_INCOMPLETE_DATA (ESP_ERR_HTTP_BASE + 12)
#define ESP_ERR_HTTP_HEADER_TOO_LONG (ESP_ERR_HTTP_BASE + 14)

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client, int *code, int *flags);
