# demorig for M5Stack Tab5 (ESP32-P4, 720x1280 MIPI-DSI, touch). See example/m5common/README.md.
#
#   ./build.sh    # or: source $IDF_ROOT/esp-idf/export.sh; idf.py build
#   ./run.sh      # build and flash
#   ./monitor.sh  # serial log (frame rate and timings every 2 seconds)
#
# The scene (example/common/demorig/), ShapoGFX and the M5Stack front end
# are components under example/m5common/components/.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../m5common/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demorig)
