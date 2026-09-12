idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp-openclaw-room-node tab5_room_board
    PRIV_REQUIRES esp-openclaw-node heap freertos)

idf_component_get_property(node_component_dir esp-openclaw-node COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${node_component_dir}/private_include")
