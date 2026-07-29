idf_component_register(
    SRCS "src/esp_openclaw_talk.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-openclaw-node esp_http_client esp_webrtc esp_peer json)
