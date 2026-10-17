#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct {
    uint32_t tx_buffer_size;
    uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg);
bool usb_serial_jtag_is_connected(void);
int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks);
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks);
