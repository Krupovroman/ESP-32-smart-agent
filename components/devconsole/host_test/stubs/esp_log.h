#pragma once
#include <stdarg.h>
// Logs go through the hook installed with esp_log_set_vprintf(), as on the device.
typedef int (*vprintf_like_t)(const char *fmt, va_list ap);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t f);
void host_log(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, ...) host_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
