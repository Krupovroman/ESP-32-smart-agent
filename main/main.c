#include "ai_client.h"
#include "ai_cmds.h"
#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "devconsole.h"
#include "display.h"
#include "inject.h"
#include "net.h"
#include "ota.h"
#include "selftest.h"
#include "settings.h"
#include "touch.h"
#include "ui_test.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    board_power_latch_on();
    // after repeated crashes during boot only the console and the display come up
    bool safe = selftest_boot_begin();
    ESP_ERROR_CHECK(settings_init());
    // the console comes this early so a board that fails later is still reachable. Its own
    // failure is not fatal (an abort here would boot-loop even in safe mode); the self-test
    // rejects a pending OTA image without a console.
    ai_console_register();   // answers "not ready" until selftest_ready() (and in safe mode)
    if (devconsole_start() != ESP_OK) {
        ESP_LOGE(TAG, "dev console unavailable");
    }

    selftest_inputs_t st = {
        .audio_in = ESP_ERR_INVALID_STATE, .audio_out = ESP_ERR_INVALID_STATE, .wifi = ESP_ERR_INVALID_STATE,
    };
    // in safe mode a broken board or panel must not abort: the console has to stay reachable
    st.board = board_init();
    if (!safe) {
        ESP_ERROR_CHECK(st.board);
    }
    devconsole_heap_stage("after_board");
    st.display = st.board == ESP_OK ? display_init() : ESP_ERR_INVALID_STATE;
    if (!safe) {
        ESP_ERROR_CHECK(st.display);
    }
    devconsole_heap_stage("after_display");
    bool have_ui = st.display == ESP_OK;
    st.touch = have_ui ? touch_init(display_get()) : ESP_ERR_INVALID_STATE;
    if (st.touch != ESP_OK) {
        ESP_LOGE(TAG, "touch unavailable");
    }
    if (have_ui) {
        inject_init(display_get());
    }
    if (!safe) {
        st.audio_in = audio_in_init();
        if (st.audio_in != ESP_OK) {
            ESP_LOGE(TAG, "microphone unavailable");
        }
        st.audio_out = audio_out_init();
        if (st.audio_out != ESP_OK) {
            ESP_LOGE(TAG, "speaker output unavailable");
        }
        audio_out_set_volume((uint8_t)settings_get_int("volume"));
        audio_in_set_format(settings_get_int("mic_slot") == 0, settings_get_int("mic_shift"));
        devconsole_heap_stage("after_audio");
    }

    if (have_ui) {
        ui_test_create();
        vTaskDelay(pdMS_TO_TICKS(80));   // let the first frame reach the panel before lighting it
        board_backlight_set((uint8_t)settings_get_int("bright"));
    }

    if (!safe) {
        st.wifi = net_init();
        if (st.wifi != ESP_OK) {
            ESP_LOGE(TAG, "wifi unavailable");
        }
        devconsole_heap_stage("after_wifi_init");
        // a pending OTA image is confirmed here, or rolled back (no return)
        selftest_run(&st);
        if (st.wifi == ESP_OK) {
            net_start();
        }
        if (ai_client_init() != ESP_OK) {
            ESP_LOGE(TAG, "ai client unavailable");
        }
    } else {
        ota_selftest_skipped("safe mode");
    }
    selftest_ready();
}
