idf_component_register(
    SRCS "esp_openclaw_directory.c" "esp_openclaw_avatar.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    set(avatar_generator "${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_agent_avatars.py")
    set(avatar_rasterizer "${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate_watch_lobster.py")
    set(avatar_assets "${CMAKE_CURRENT_BINARY_DIR}/agent_avatars")
    add_custom_command(
        OUTPUT "${avatar_assets}/agent_avatars.argb8888"
        COMMAND "${python}" "${avatar_generator}" --output-dir "${avatar_assets}"
        DEPENDS "${avatar_generator}" "${avatar_rasterizer}"
        VERBATIM)
    add_custom_target(openclaw_agent_avatar_assets
        DEPENDS "${avatar_assets}/agent_avatars.argb8888")
    add_dependencies(${COMPONENT_LIB} openclaw_agent_avatar_assets)
    target_add_binary_data(${COMPONENT_LIB} "${avatar_assets}/agent_avatars.argb8888" BINARY)
endif()
