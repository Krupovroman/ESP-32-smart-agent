#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) { host_log("E", tag, "check failed: " #a); return (err); } } while (0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t e__ = (x); if (e__ != ESP_OK) { return e__; } } while (0)
