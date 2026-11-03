idf_component_register(
    SRCS "openclaw_status_bar.c" "openclaw_connection_details.c" "openclaw_reading.c" "openclaw_density.c" "openclaw_preferences.c" "openclaw_setup.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash lvgl__lvgl esp-openclaw-node-provisioning
)
