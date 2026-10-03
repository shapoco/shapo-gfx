# Native build of the demo2d sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
set(DEMO2D_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../common/demo2d)
add_executable(demo2d main.cpp ${DEMO2D_COMMON}/scene.cpp)
target_include_directories(demo2d PRIVATE ${DEMO2D_COMMON})
target_link_libraries(demo2d PRIVATE shapoco::gfx)
