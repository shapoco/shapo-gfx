# Native build of the demorig sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
add_executable(demorig main.cpp scene.cpp)
target_link_libraries(demorig PRIVATE shapoco::gfx)
