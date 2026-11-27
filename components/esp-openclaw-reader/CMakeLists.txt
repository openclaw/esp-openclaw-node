idf_component_register(
    SRCS "esp_openclaw_reader.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-openclaw-node esp-openclaw-directory esp-openclaw-chat
             esp_timer freertos espressif__cjson
)
