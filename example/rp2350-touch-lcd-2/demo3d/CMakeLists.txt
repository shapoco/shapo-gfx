# demo3d on the Waveshare RP2350-Touch-LCD-2 (320x240 landscape, two cores,
# touch). See example/rp2350-touch-lcd-2/README.md.
#
#   ../build.sh            # cmake -B build && cmake --build build
#   ../flash.sh [build/demo3d.uf2]

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
include(${CMAKE_CURRENT_LIST_DIR}/../../rp2common/pico_project.cmake)

project(demo3d C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# -DDEMO3D_HOT=OFF leaves the rasterizer in flash (it runs from RAM by
# default: SHAPOGFX3D_HOT_ATTR, see docsrc/intro/getting_started.rst)
option(DEMO3D_HOT "Run the span rasterizer of gfx3d from RAM" ON)
# Measurements on stdio every 2 seconds (main.cpp, "Profile")
option(DEMO3D_PROFILE "Measure where a frame goes" OFF)
# All the code of the 3D renderer (src/gfx3d/: gfx3d.cpp, scene.cpp,
# shapes.cpp, and gfx2d's shapes.cpp, an archive member of the same name)
# and of the demo's scene in RAM, by a linker script derived from the SDK's.
# From flash through the 16 KB XIP cache the scene took 12.3 ms to build
# instead of 5.4 (31 fps instead of 41, 2026-10-03); this costs about 62 KB
# of RAM.
option(DEMO3D_GFX3D_IN_RAM "Run the 3D renderer's code from RAM" ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../rp2common rp2common)
if(DEMO3D_HOT)
    target_compile_definitions(shapogfx PRIVATE
        "SHAPOGFX3D_HOT_ATTR=__attribute__((section(\".time_critical.gfx3d\")))"
        SHAPOGFX3D_HOT_INSTANTIATE=1)
endif()

# The scene: example/common/demo3d/ (shared with the WASM build)
set(DEMO3D_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../common/demo3d)
add_executable(demo3d main.cpp ${DEMO3D_COMMON}/scene.cpp
    ${DEMO3D_COMMON}/demo3d.cpp)
target_include_directories(demo3d PRIVATE ${DEMO3D_COMMON})
target_link_libraries(demo3d PRIVATE rp2common)
if(DEMO3D_PROFILE)
    target_compile_definitions(demo3d PRIVATE DEMO3D_PROFILE=1)
endif()
if(DEMO3D_GFX3D_IN_RAM)
    # The flash .text rule leaves those files out, so the catch-all
    # *(.text*) of .data (copied to RAM at startup) takes them
    set(SDK_LD ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld)
    file(READ ${SDK_LD} LD)
    set(FLASH_RULE "*(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)")
    string(FIND "${LD}" "${FLASH_RULE}" FOUND)
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "DEMO3D_GFX3D_IN_RAM: the SDK's linker script changed")
    endif()
    string(REPLACE "${FLASH_RULE}"
        "*(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a: *libshapogfx.a:gfx3d.cpp.o *libshapogfx.a:scene.cpp.o *libshapogfx.a:shapes.cpp.o *common/demo3d/scene.cpp.o) .text*)"
        LD "${LD}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_gfx3d_in_ram.ld "${LD}")
    pico_set_linker_script(demo3d ${CMAKE_CURRENT_BINARY_DIR}/memmap_gfx3d_in_ram.ld)
endif()
rp2common_setup(demo3d)
