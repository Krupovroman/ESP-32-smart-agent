#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) \
    do {                                      \
        if (!(a)) {                           \
            return (err);                     \
        }                                     \
    } while (0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) \
    do {                                 \
        esp_err_t err_rc_ = (x);         \
        if (err_rc_ != ESP_OK) {         \
            return err_rc_;              \
        }                                \
    } while (0)
