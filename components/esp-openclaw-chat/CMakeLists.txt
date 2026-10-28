idf_component_register(
    SRCS "src/esp_openclaw_chat.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-openclaw-node espressif__cjson esp_timer freertos
)
