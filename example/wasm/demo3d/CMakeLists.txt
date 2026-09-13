# Native build of the demo3d sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
add_executable(demo3d main.cpp scene.cpp)
target_link_libraries(demo3d PRIVATE shapoco::gfx)
