cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ../../components/esp-openclaw-node ../../components/esp-openclaw-node-provisioning)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openclaw_m5stack_dial_node)
