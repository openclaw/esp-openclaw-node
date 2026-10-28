idf_component_register(
    SRCS "app_main.c" "dial_action.c" "dial_control.c" "dial_home.c" "dial_input.c" "dial_reader.c" "dial_navigation.c"
    INCLUDE_DIRS "."
    REQUIRES esp-openclaw-node esp-openclaw-node-provisioning esp-openclaw-directory esp-openclaw-ui m5dial
             esp_event esp_netif esp_timer esp_driver_gpio esp_driver_ledc
             espressif__cjson nvs_flash console
)
