#pragma once
// Pin map of the Waveshare ESP32-S3-Touch-LCD-1.85 (see docs/hardware.md)
#include "driver/gpio.h"

#define BOARD_LCD_H_RES         360
#define BOARD_LCD_V_RES         360

#define BOARD_PIN_PWR_LATCH     GPIO_NUM_7   // drive high to stay powered on battery
#define BOARD_PIN_PWR_KEY       GPIO_NUM_6   // active low
#define BOARD_PIN_BOOT_KEY      GPIO_NUM_0   // active low
#define BOARD_PIN_BAT_ADC       GPIO_NUM_8   // ADC1 ch7, x3 divider

#define BOARD_I2C0_PORT         0            // TCA9554, PCF85063, QMI8658 (+ header)
#define BOARD_PIN_I2C0_SDA      GPIO_NUM_11
#define BOARD_PIN_I2C0_SCL      GPIO_NUM_10

#define BOARD_I2C1_PORT         1            // CST816 touch only
#define BOARD_PIN_TP_SDA        GPIO_NUM_1
#define BOARD_PIN_TP_SCL        GPIO_NUM_3
#define BOARD_PIN_TP_INT        GPIO_NUM_4

#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_PIN_LCD_CS        GPIO_NUM_21
#define BOARD_PIN_LCD_SCK       GPIO_NUM_40
#define BOARD_PIN_LCD_D0        GPIO_NUM_46
#define BOARD_PIN_LCD_D1        GPIO_NUM_45
#define BOARD_PIN_LCD_D2        GPIO_NUM_42
#define BOARD_PIN_LCD_D3        GPIO_NUM_41
#define BOARD_PIN_LCD_TE        GPIO_NUM_18
#define BOARD_PIN_LCD_BL        GPIO_NUM_5

#define BOARD_PIN_SPK_BCLK      GPIO_NUM_48
#define BOARD_PIN_SPK_LRCK      GPIO_NUM_38
#define BOARD_PIN_SPK_DOUT      GPIO_NUM_47

#define BOARD_PIN_MIC_SCK       GPIO_NUM_15
#define BOARD_PIN_MIC_WS        GPIO_NUM_2
#define BOARD_PIN_MIC_SD        GPIO_NUM_39

// TCA9554 pins (EXIOn = Pn-1)
#define BOARD_EXIO_TP_RST       (1u << 0)    // EXIO1
#define BOARD_EXIO_LCD_RST      (1u << 1)    // EXIO2
#define BOARD_EXIO_SD_CS        (1u << 2)    // EXIO3
#define BOARD_EXIO_IMU_INT2     (1u << 3)    // EXIO4
#define BOARD_EXIO_IMU_INT1     (1u << 4)    // EXIO5
