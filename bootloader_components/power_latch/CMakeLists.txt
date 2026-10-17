# Picked up by the bootloader build only (components/bootloader/subproject adds
# <project>/bootloader_components). Same pattern as the IDF custom_bootloader/bootloader_hooks example.
idf_component_register(SRCS "power_latch.c"
                       REQUIRES esp_rom soc)
