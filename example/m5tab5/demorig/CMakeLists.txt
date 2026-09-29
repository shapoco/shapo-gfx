# demorig for M5Stack Tab5 (ESP32-P4, 720x1280 MIPI-DSI, touch). See example/m5common/README.md.
#
#   ./build.sh    # or: source $IDF_ROOT/esp-idf/export.sh; idf.py build
#   ./run.sh      # build and flash
#   ./monitor.sh  # serial log (frame rate and timings every 2 seconds)
#
# The scene (example/common/demorig/) and the M5Stack front end are
# components under example/m5common/components/, and ShapoGFX is this
# repository itself. A project of its own would write
# set(EXTRA_COMPONENT_DIRS $ENV{SHAPOGFX_PATH}).

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../../m5common/components
    ${CMAKE_CURRENT_LIST_DIR}/../../..)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demorig)
