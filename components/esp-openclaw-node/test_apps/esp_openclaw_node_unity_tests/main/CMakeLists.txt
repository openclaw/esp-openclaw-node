idf_component_register(
    SRCS
        "test_esp_openclaw_node.c"
        "../../../../esp-openclaw-talk/tests/test_esp_openclaw_talk.c"
    INCLUDE_DIRS "." "../../../../esp-openclaw-talk/include"
    REQUIRES
        esp_http_client
        json
        mbedtls
        nvs_flash
        esp-openclaw-node
        unity
)

idf_component_get_property(esp_openclaw_node_dir esp-openclaw-node COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_openclaw_node_dir}/private_include")
target_include_directories(
    ${COMPONENT_LIB}
    PRIVATE
        "../../../../../third_party/esp-webrtc-solution/components/esp_webrtc/include"
        "../../../../../third_party/esp-webrtc-solution/components/esp_peer/include")
