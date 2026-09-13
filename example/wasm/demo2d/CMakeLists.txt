# Native build of the demo2d sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
add_executable(demo2d main.cpp scene.cpp)
target_link_libraries(demo2d PRIVATE shapoco::gfx)
