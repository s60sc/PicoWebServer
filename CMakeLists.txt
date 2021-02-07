cmake_minimum_required(VERSION 3.12)
include(pico_sdk_import.cmake)
project(s60scProject C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(MYPROJECTS_PATH ${PROJECT_SOURCE_DIR})
pico_sdk_init()

add_subdirectory(PicoWebServer)
#add_subdirectory(double_tap_usb_boot)

