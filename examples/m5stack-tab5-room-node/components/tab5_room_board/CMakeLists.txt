idf_component_register(
    SRCS "tab5_room_board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp-openclaw-node-provisioning
        esp-openclaw-room-node
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ppa
        esp_driver_uart
        esp_lcd
        esp_mm
        espressif__esp_video
        esp_timer
        heap
        espressif__cjson
        espressif__esp_codec_dev
        espressif__esp_new_jpeg
        espressif__esp_lcd_touch_st7123
        espressif__esp_lvgl_port
        m5stack_tab5
        mbedtls
        tab5_st7121)
