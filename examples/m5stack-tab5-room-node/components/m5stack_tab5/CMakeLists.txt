if(CMAKE_SCRIPT_MODE_FILE)
    idf_component_register(
        REQUIRES
            esp_driver_gpio esp_driver_i2c esp_driver_i2s spiffs fatfs vfs
            esp_driver_sdspi esp_driver_sdmmc esp_codec_dev esp_io_expander
            esp_io_expander_pi4ioe5v6408 esp_lvgl_port esp_lcd_st7123
            esp_lcd_ili9881c esp_lcd_touch_st7123 esp_lcd_touch_gt911
            esp_video usb bmi270)
    return()
endif()

include(FetchContent)

# m5stack_tab5 1.2.0~1's registry manifest pins esp_video ~2.0, while the
# P4-capable esp_capture used by Talk requires ^2.1. Consume the exact BSP
# source commit without copying or patching it; dependencies are declared by
# the example at mutually compatible versions.
set(OPENCLAW_TAB5_BSP_LOCAL_PATH "" CACHE PATH
    "Explicit clean esp-bsp checkout at f0ef9497... (normally leave empty)")
if(NOT OPENCLAW_TAB5_BSP_LOCAL_PATH AND DEFINED ENV{OPENCLAW_TAB5_BSP_LOCAL_PATH})
    set(OPENCLAW_TAB5_BSP_LOCAL_PATH "$ENV{OPENCLAW_TAB5_BSP_LOCAL_PATH}")
endif()
if(OPENCLAW_TAB5_BSP_LOCAL_PATH)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${OPENCLAW_TAB5_BSP_LOCAL_PATH}"
        OUTPUT_VARIABLE tab5_local_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE tab5_git_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${OPENCLAW_TAB5_BSP_LOCAL_PATH}"
        OUTPUT_VARIABLE tab5_local_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE tab5_status_result)
    if(NOT tab5_git_result EQUAL 0 OR NOT tab5_status_result EQUAL 0 OR
       NOT tab5_local_head STREQUAL "f0ef9497efce684997ce391edd19733483e250a5" OR
       NOT tab5_local_status STREQUAL "")
        message(FATAL_ERROR
            "OPENCLAW_TAB5_BSP_LOCAL_PATH must be a clean checkout at "
            "f0ef9497efce684997ce391edd19733483e250a5")
    endif()
    set(tab5_bsp_source_SOURCE_DIR "${OPENCLAW_TAB5_BSP_LOCAL_PATH}")
else()
    FetchContent_Declare(
        tab5_bsp_source
        URL https://github.com/espressif/esp-bsp/archive/f0ef9497efce684997ce391edd19733483e250a5.tar.gz
        URL_HASH SHA256=402d60906d3cc418637f551323b0eae980e011e3a6afa09b45b796d2f757f699
        DOWNLOAD_EXTRACT_TIMESTAMP true)
    FetchContent_GetProperties(tab5_bsp_source)
    if(NOT tab5_bsp_source_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(PUSH)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(tab5_bsp_source)
        if(POLICY CMP0169)
            cmake_policy(POP)
        endif()
    endif()
endif()
set(tab5_component_dir "${tab5_bsp_source_SOURCE_DIR}/bsp/m5stack_tab5")
file(GLOB_RECURSE tab5_sources "${tab5_component_dir}/src/*.c")
idf_component_register(
    SRCS ${tab5_sources}
    INCLUDE_DIRS "${tab5_component_dir}/include"
    PRIV_INCLUDE_DIRS "${tab5_component_dir}/priv_include"
    REQUIRES
        esp_driver_gpio esp_driver_i2c esp_driver_i2s spiffs fatfs vfs
        esp_driver_sdspi esp_driver_sdmmc esp_codec_dev esp_io_expander
        esp_io_expander_pi4ioe5v6408 esp_lvgl_port esp_lcd_st7123
        esp_lcd_ili9881c esp_lcd_touch_st7123 esp_lcd_touch_gt911
        esp_video usb bmi270
    PRIV_REQUIRES esp_psram esp_lcd esp_driver_ledc)
