idf_component_register(
    SRCS "main.c" "room_media.c" "room_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        av_render
        esp-openclaw-node
        esp-openclaw-talk
        esp_event
        esp_netif
        esp_peer
        esp_webrtc
        esp_wifi
        espressif__esp-sr
        espressif__esp_audio_codec
        espressif__esp_capture
        espressif__esp_codec_dev
        freertos
        lvgl__lvgl
        media_lib_utils
        nvs_flash
        waveshare__esp32_s3_touch_amoled_2_06)
