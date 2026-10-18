idf_component_register(
    SRC_DIRS "." "app" "ui" "ui/fonts"
    INCLUDE_DIRS "." "app" "ui"
    PRIV_REQUIRES board display touch audio settings net orhttp devconsole ota ai assets
                  app_update esp_timer lvgl__lvgl espressif__esp_lvgl_port
)

# LVGL's built-in TLSF heap gets its 1 MB pool from PSRAM (architecture 6, same mechanism as xiaozhi)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ui")
    target_compile_definitions(${lvgl_lib} PRIVATE "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
endif()

# Storage asset pack (architecture 1.0 "storage"): the CJK fallback font of the translator. Built with every build
# when its inputs change (build/storage.bin), never part of "idf.py flash"; "idf.py -p COM7 storage-flash" writes
# it to the storage partition.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(project_dir PROJECT_DIR)
    idf_build_get_property(python PYTHON)
    set(pack_img "${CMAKE_BINARY_DIR}/storage.bin")
    set(pack_font "${project_dir}/assets/fonts/cjk/RACJK-Medium.ttf")
    set(pack_lic "${project_dir}/assets/fonts/NotoSansCJK-OFL.txt")
    add_custom_command(OUTPUT "${pack_img}"
        COMMAND ${python} "${project_dir}/tools/mkpack.py" --font "${pack_font}" --license "${pack_lic}"
                -o "${pack_img}"
        DEPENDS "${project_dir}/tools/mkpack.py" "${pack_font}" "${pack_lic}"
        COMMENT "Building the storage asset pack (CJK font)"
        VERBATIM)
    add_custom_target(storage_pack ALL DEPENDS "${pack_img}")
    esp_partition_register_target(storage "${pack_img}" ALWAYS_PLAINTEXT DEPENDS storage_pack)
endif()
