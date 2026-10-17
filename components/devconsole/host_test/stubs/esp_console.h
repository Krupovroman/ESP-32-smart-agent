#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int (*esp_console_cmd_func_t)(int argc, char **argv);
typedef int (*esp_console_cmd_func_with_context_t)(void *context, int argc, char **argv);
typedef struct {
    const char *command;
    const char *help;
    const char *hint;
    esp_console_cmd_func_t func;
    void *argtable;
    esp_console_cmd_func_with_context_t func_w_context;
    void *context;
} esp_console_cmd_t;
typedef struct {
    size_t max_cmdline_length;
    size_t max_cmdline_args;
    uint32_t heap_alloc_caps;
    int hint_color;
    int hint_bold;
} esp_console_config_t;
#define ESP_CONSOLE_CONFIG_DEFAULT() { 256, 32, 0, 39, 0 }
esp_err_t esp_console_init(const esp_console_config_t *cfg);
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);
esp_err_t esp_console_register_help_command(void);
// Splits on spaces (no quoting) and runs the command; ESP_ERR_NOT_FOUND if unknown.
esp_err_t esp_console_run(const char *cmdline, int *cmd_ret);
