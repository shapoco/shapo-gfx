# Native build of the demo3d sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
set(DEMO3D_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../common/demo3d)
add_executable(demo3d main.cpp ${DEMO3D_COMMON}/scene.cpp)
target_include_directories(demo3d PRIVATE ${DEMO3D_COMMON})
target_link_libraries(demo3d PRIVATE shapoco::gfx)
