set(esp_openclaw_node_component_args
    SRCS
        "src/esp_openclaw_node_identity.c"
        "src/esp_openclaw_node_persisted_session.c"
        "src/esp_openclaw_node.c"
        "src/esp_openclaw_node_connect_source.c"
        "src/esp_openclaw_node_registry.c"
        "src/esp_openclaw_node_protocol.c"
        "src/esp_openclaw_node_handshake.c"
        "src/esp_openclaw_node_transport.c"
        "src/esp_openclaw_node_runtime.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES
        espressif__cjson
        esp_app_format
        esp_websocket_client
        libsodium
        mbedtls
        nvs_flash
)

idf_component_register(${esp_openclaw_node_component_args})

# Work around the IDF 6.0/newlib-picolibc Annex K header breakage in the
# managed libsodium component without affecting the rest of the build.
if(TARGET __idf_espressif__libsodium)
    target_compile_definitions(__idf_espressif__libsodium PRIVATE __STDC_WANT_LIB_EXT1__=0)
endif()
