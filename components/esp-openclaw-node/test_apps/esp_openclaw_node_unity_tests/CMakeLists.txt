cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ../../)
set(COMPONENTS main)

list(APPEND SDKCONFIG_DEFAULTS
    "$ENV{IDF_PATH}/tools/test_apps/configs/sdkconfig.debug_helpers"
    "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_openclaw_node_unity_tests)
