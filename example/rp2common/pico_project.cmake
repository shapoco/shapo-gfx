# Included by the RP2 example projects (example/rp2350-touch-lcd-2/*/) before
# project(): selects the board and imports the Pico SDK.
#
#   set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
#   include(<this file>)
#   project(name C CXX ASM)
#   pico_sdk_init()
#   add_subdirectory(<example/rp2common> rp2common)
#
# The SDK is taken from $PICO_SDK_PATH, or ~/pico/pico-sdk when it is not set.
# picotool (which makes the .uf2) from $picotool_DIR, or else the newest one
# the VS Code extension installed under ~/.pico-sdk/picotool/ (otherwise the
# SDK downloads and builds it).

if(NOT PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        set(PICO_SDK_PATH $ENV{HOME}/pico/pico-sdk)
    endif()
endif()
set(PICO_SDK_PATH ${PICO_SDK_PATH} CACHE PATH "Path to the Pico SDK")

# The board headers of example/rp2common/boards/ besides the SDK's
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
if(NOT PICO_BOARD)
    message(FATAL_ERROR "set PICO_BOARD before including pico_project.cmake")
endif()
# The Cortex-M33 cores (the FPU builds of ShapoGFX)
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT picotool_DIR AND NOT DEFINED ENV{picotool_DIR})
    file(GLOB RP2COMMON_PICOTOOLS LIST_DIRECTORIES true
        $ENV{HOME}/.pico-sdk/picotool/*/picotool)
    if(RP2COMMON_PICOTOOLS)
        list(SORT RP2COMMON_PICOTOOLS)
        list(GET RP2COMMON_PICOTOOLS -1 RP2COMMON_PICOTOOL)
        set(picotool_DIR ${RP2COMMON_PICOTOOL} CACHE PATH "picotool package")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
