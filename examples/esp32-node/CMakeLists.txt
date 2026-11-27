cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ../../components/esp-openclaw-node ../../components/esp-openclaw-node-provisioning
    ../../components/esp-openclaw-reader ../../components/esp-openclaw-directory
    ../../components/esp-openclaw-chat ../../components/esp-openclaw-ui)
set(COMPONENTS main)
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_openclaw_node_esp32)
