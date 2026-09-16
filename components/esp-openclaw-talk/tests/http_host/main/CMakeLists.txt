get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(TALK_SOURCE "${REPO}/components/esp-openclaw-talk/src/esp_openclaw_talk.c" CACHE FILEPATH "Production Talk source")
set(CJSON_DIR "${REPO}/components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests/managed_components/espressif__cjson/cJSON" CACHE PATH "Configured cJSON source")
set(WEBRTC_DIR "${REPO}/third_party/esp-webrtc-solution" CACHE PATH "Pinned WebRTC checkout")
idf_component_register(SRCS main.c "${TALK_SOURCE}" "${CJSON_DIR}/cJSON.c"
    INCLUDE_DIRS "${CJSON_DIR}" "${REPO}/components/esp-openclaw-talk/include"
        "${REPO}/components/esp-openclaw-node/include"
        "${WEBRTC_DIR}/components/esp_webrtc/include"
        "${WEBRTC_DIR}/components/esp_peer/include"
    REQUIRES esp_http_client freertos esp_timer mbedtls)
target_compile_options(${COMPONENT_LIB} PRIVATE -UNDEBUG)
