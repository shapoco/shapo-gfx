# Native build of the demorig sample (renders one frame to a PPM file).
# The browser (WASM) build uses the Makefile in this directory instead.
set(DEMORIG_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../../common/demorig)
add_executable(demorig main.cpp ${DEMORIG_COMMON}/demorig.cpp ${DEMORIG_COMMON}/scene.cpp)
target_include_directories(demorig PRIVATE ${DEMORIG_COMMON})
target_link_libraries(demorig PRIVATE shapoco::gfx)
