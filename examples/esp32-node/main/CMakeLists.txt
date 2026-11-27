set(board_requires esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc
    lvgl__lvgl esp-openclaw-reader esp-openclaw-ui)
# Component requirements are expanded before sdkconfig choices are available.
if(IDF_TARGET STREQUAL "esp32")
    list(APPEND board_requires waveshare__esp_lcd_st7735)
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "esp_openclaw_node_example_adc_node_cmd.c"
        "esp_openclaw_node_example_cmd.c"
        "esp_openclaw_node_example_gpio_node_cmd.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp-openclaw-node
        esp-openclaw-node-provisioning
        esp_adc
        esp_app_format
        console
        esp_driver_gpio
        esp_event
        esp_netif
        esp_wifi
        espressif__cjson
        mbedtls
        nvs_flash
        ${board_requires}
)

if(CONFIG_EXAMPLE_BOARD_M5STICKC OR CONFIG_EXAMPLE_BOARD_T_EMBED)
    target_sources(${COMPONENT_LIB} PRIVATE "compact_board.c" "compact_workspace.c" "compact_input.c")
endif()
