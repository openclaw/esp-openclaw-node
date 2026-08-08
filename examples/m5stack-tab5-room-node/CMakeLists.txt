cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components/esp-openclaw-node"
    "../../components/esp-openclaw-node-provisioning"
    "../../components/esp-openclaw-talk"
    "../../components/esp-openclaw-room-node")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openclaw_m5stack_tab5_room_node)
