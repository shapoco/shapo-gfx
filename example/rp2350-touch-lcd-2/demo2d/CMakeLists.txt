# demo2d on the Waveshare RP2350-Touch-LCD-2 (320x240 landscape, two cores).
# See example/rp2350-touch-lcd-2/README.md.
#
#   ../build.sh            # cmake -B build && cmake --build build
#   ../flash.sh [build/demo2d.uf2]

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
include(${CMAKE_CURRENT_LIST_DIR}/../../rp2common/pico_project.cmake)

project(demo2d C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../rp2common rp2common)

# The scene: example/common/demo2d/ (shared with the WASM build)
set(DEMO2D_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../common/demo2d)
add_executable(demo2d main.cpp ${DEMO2D_COMMON}/scene.cpp)
target_include_directories(demo2d PRIVATE ${DEMO2D_COMMON})
target_link_libraries(demo2d PRIVATE rp2common)
rp2common_setup(demo2d)
