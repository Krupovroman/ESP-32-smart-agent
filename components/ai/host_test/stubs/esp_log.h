#pragma once
void host_log(const char *lvl, const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, ...) host_log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
