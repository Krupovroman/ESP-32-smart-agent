// Bootloader hook: drive GPIO7 (power latch) high as the very first thing after the ROM
// loader, so a board started from battery by the PWR key stays on through the whole boot
// (docs/architecture.md §1.2, §8.1). The app latches it again in board_power_latch_on().
#include <stdbool.h>
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"

#define PWR_LATCH_GPIO 7

void bootloader_hooks_include(void);
void bootloader_before_init(void);

// Referenced so the linker keeps this file (the bootloader's hooks are weak symbols).
void bootloader_hooks_include(void)
{
}

// No BSS, flash or logging yet: ROM calls and registers only. Level first, then the driver
// is enabled, so the pin never drives low.
void bootloader_before_init(void)
{
    esp_rom_gpio_pad_select_gpio(PWR_LATCH_GPIO);
    esp_rom_gpio_connect_out_signal(PWR_LATCH_GPIO, SIG_GPIO_OUT_IDX, false, false);
    REG_WRITE(GPIO_OUT_W1TS_REG, BIT(PWR_LATCH_GPIO));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(PWR_LATCH_GPIO));
}
