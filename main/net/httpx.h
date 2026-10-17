#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

// HTTPS client with the Mozilla CA bundle, keep-alive, and (when api_key is set) the
// Authorization / OpenRouter attribution headers.
esp_http_client_handle_t httpx_client(const char *url, esp_http_client_method_t method, const char *api_key,
                                      int timeout_ms);

// GET url, read and discard the body. Fills HTTP status, body size and total time.
esp_err_t httpx_probe(const char *url, const char *api_key, int *status, size_t *bytes, int *ms);
