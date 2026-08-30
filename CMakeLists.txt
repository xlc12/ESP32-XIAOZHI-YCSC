# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.9.4")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# 监控音频/板级文件目录变化，增删文件时自动触发 reconfigure
# ESP-IDF 组件 CMakeLists 中不能用 CONFIGURE_DEPENDS，故在顶层做全局监控
file(GLOB_RECURSE XIAOZHI_WATCH_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/main/boards/*/sounds/*.ogg
    ${CMAKE_CURRENT_SOURCE_DIR}/main/assets/locales/*/*.ogg
    ${CMAKE_CURRENT_SOURCE_DIR}/main/assets/common/*.ogg
    ${CMAKE_CURRENT_SOURCE_DIR}/main/boards/*/config.h
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

