# demorig on the Waveshare RP2350-Touch-LCD-2 (320x240 landscape, two cores,
# touch). See example/rp2350-touch-lcd-2/README.md.
#
#   ../build.sh            # cmake -B build && cmake --build build
#   ../flash.sh [build/demorig.uf2]

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
include(${CMAKE_CURRENT_LIST_DIR}/../../rp2common/pico_project.cmake)

project(demorig C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# The character: the atlas (default: a power-of-two stride, so the turned
# parts take the SIO interpolator path; 28.6 fps against 28.0 at zoom 1,
# 2026-10-03) or one texture per image (-DDEMORIG_ATLAS=OFF, as on the
# M5Stack: fewer bytes read from flash per frame)
option(DEMORIG_ATLAS "Use the atlas model (rgb_chan.hpp)" ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../rp2common rp2common)

# The scene, the view and the overlay: example/common/demorig/ (shared with
# the WASM and M5Stack builds)
set(DEMORIG_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../common/demorig)
add_executable(demorig main.cpp ${DEMORIG_COMMON}/demorig.cpp
    ${DEMORIG_COMMON}/scene.cpp)
target_include_directories(demorig PRIVATE ${DEMORIG_COMMON})
if(NOT DEMORIG_ATLAS)
    set_source_files_properties(${DEMORIG_COMMON}/scene.cpp PROPERTIES
        COMPILE_DEFINITIONS "DEMORIG_MODEL_HEADER=\"model/rgb_chan_sep.hpp\"")
endif()
target_link_libraries(demorig PRIVATE rp2common)
rp2common_setup(demorig)
