idf_component_register(
    SRCS
        "esp_openclaw_room_node.c"
        "room_agent_directory.c"
        "room_aec_src.c"
        "room_audio_gate.c"
        "room_board.c"
        "room_canvas_a2ui_store.c"
        "room_canvas_http_decode.c"
        "room_canvas_lvgl_renderer.c"
        "room_canvas_snapshot.c"
        "room_canvas_surface_refresh.c"
        "room_canvas_node_cmd.c"
        "room_device_commands.c"
        "room_diagnostics.c"
        "room_diagnostics_data.c"
        "room_diagnostics_metrics.c"
        "room_face.c"
        "room_lobster.c"
        "room_file_validation.c"
        "room_files.c"
        "room_media.c"
        "room_pcm_gain.c"
        "room_recording.c"
        "room_ui_controller.c"
        "room_voice_draft.c"
        "room_watch_home.c"
        "room_watch_navigation.c"
        "room_watch_settings.c"
        "room_watch_agents.c"
        "room_watch_devices.c"
        "room_watch_conversations.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    EMBED_FILES "assets/openclaw_lobster.argb8888"
    REQUIRES
        av_render
        console
        esp-openclaw-node
        esp-openclaw-directory
        esp-openclaw-ui
        esp-openclaw-node-provisioning
        esp-openclaw-talk
        esp_app_format
        esp_event
        esp_http_client
        esp_lcd
        esp_netif
        esp_peer
        esp_timer
        esp_webrtc
        esp_wifi
        espressif__cjson
        espressif__esp-sr
        espressif__esp_audio_codec
        espressif__esp_audio_effects
        espressif__esp_capture
        espressif__esp_codec_dev
        espressif__esp_new_jpeg
        freertos
        json
        lvgl__lvgl
        mbedtls
        media_lib_utils
        nvs_flash)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    set(lobster_generator "${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_watch_lobster.py")
    set(lobster_assets "${CMAKE_CURRENT_BINARY_DIR}/watch_lobster")
    add_custom_command(
        OUTPUT "${lobster_assets}/watch_lobster.argb8888" "${lobster_assets}/watch_lobster_parts.h"
        COMMAND "${python}" "${lobster_generator}" --output-dir "${lobster_assets}"
        DEPENDS "${lobster_generator}"
        VERBATIM)
    add_custom_target(room_watch_lobster_assets
        DEPENDS "${lobster_assets}/watch_lobster.argb8888" "${lobster_assets}/watch_lobster_parts.h")
    add_dependencies(${COMPONENT_LIB} room_watch_lobster_assets)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${lobster_assets}")
    target_add_binary_data(${COMPONENT_LIB} "${lobster_assets}/watch_lobster.argb8888" BINARY)
endif()
