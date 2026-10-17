#include "board.h"
#include "devconsole.h"
#include "display.h"
#include "inject.h"
#include "touch.h"
#include "ui_test.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// Rollback guard: a freshly OTA-updated image stays "pending" until it proves it can
// bring up the display and the LVGL loop; otherwise the bootloader returns to the old slot.
static void confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "self-test passed, confirming image in %s", running->label);
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void app_main(void)
{
    board_power_latch_on();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_init());
    if (touch_init(display_get()) != ESP_OK) {
        ESP_LOGE(TAG, "touch unavailable");
    }
    inject_init(display_get());

    ui_test_create();
    vTaskDelay(pdMS_TO_TICKS(80));   // let the first frame reach the panel before lighting it
    board_backlight_set(80);

    ESP_ERROR_CHECK(devconsole_start());
    confirm_running_image();
}
