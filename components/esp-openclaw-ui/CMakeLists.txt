idf_component_register(
    SRCS "openclaw_status_bar.c" "openclaw_connection_details.c" "openclaw_reading.c" "openclaw_density.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl esp-openclaw-node-provisioning
)
