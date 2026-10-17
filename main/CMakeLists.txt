idf_component_register(
    SRC_DIRS "." "app" "ui" "ui/fonts"
    INCLUDE_DIRS "." "app" "ui"
    PRIV_REQUIRES board display touch audio settings net orhttp devconsole ota ai
                  app_update esp_timer lvgl__lvgl espressif__esp_lvgl_port
)

# LVGL's built-in TLSF heap gets its 1 MB pool from PSRAM (architecture 6, same mechanism as xiaozhi)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ui")
    target_compile_definitions(${lvgl_lib} PRIVATE "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
endif()
