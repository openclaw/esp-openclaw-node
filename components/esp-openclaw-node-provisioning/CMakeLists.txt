idf_component_register(
    SRCS
        "esp_openclaw_node_common_device_node_cmd.c"
        "esp_openclaw_node_example_json.c"
        "esp_openclaw_node_example_repl.c"
        "esp_openclaw_node_example_repl_cmd.c"
        "esp_openclaw_node_example_saved_session_reconnect.c"
        "esp_openclaw_node_wifi.c"
        "esp_openclaw_setup_session.c"
        "esp_openclaw_setup_frame.c"
        "esp_openclaw_setup_transfer.c"
        "esp_openclaw_setup_request.c"
        "esp_openclaw_setup_response.c"
        "esp_openclaw_setup_dispatcher.c"
        "esp_openclaw_setup_security2.c"
        "esp_openclaw_setup_mailbox.c"
        "esp_openclaw_setup_wifi.c"
        "esp_openclaw_setup_gateway.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp-openclaw-node
        esp_app_format
        console
        esp_event
        esp_netif
        esp_timer
        esp_hw_support
        esp_wifi
        protocomm
        espressif__cjson
        freertos
        nvs_flash)

# Patch before compilation; the other targets never use native Security2 setup.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_IDF_TARGET_ESP32S3 AND
   CONFIG_BT_ENABLED AND CONFIG_BT_NIMBLE_ENABLED AND
   CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2 AND
   NOT CONFIG_ESP_WIFI_REMOTE_ENABLED AND NOT CONFIG_ESP_HOST_WIFI_ENABLED)
    idf_build_get_property(python PYTHON)
    execute_process(
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../../scripts/idf_srp_salt.py"
            --idf-path "$ENV{IDF_PATH}"
        RESULT_VARIABLE srp_patch_result
        OUTPUT_VARIABLE srp_patch_metadata
        ERROR_VARIABLE srp_patch_error)
    if(NOT srp_patch_result EQUAL 0)
        message(FATAL_ERROR "Security2 SDK salt guard failed: ${srp_patch_error}")
    endif()
    message(STATUS "Security2 SDK salt guard: ${srp_patch_metadata}")
endif()
