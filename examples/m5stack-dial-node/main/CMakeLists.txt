idf_component_register(
    SRCS "app_main.c" "dial_control.c" "dial_input.c"
    INCLUDE_DIRS "."
    REQUIRES esp-openclaw-node esp-openclaw-node-provisioning m5dial
             esp_event esp_netif esp_timer esp_driver_gpio esp_driver_ledc
             espressif__cjson nvs_flash console
)
