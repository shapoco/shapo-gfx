# Self-checking tests. Build with sanitizers for the strictest run, e.g.
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
#   cmake --build build && ctest --test-dir build --output-on-failure
add_executable(shapogfx_tests pixel_test.cpp graphics2d_test.cpp gfx3d_test.cpp tools_test.cpp
                              test_main.cpp)
target_include_directories(shapogfx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shapogfx_tests PRIVATE shapoco::gfx)
target_compile_options(shapogfx_tests PRIVATE -Wall -Wextra)
add_test(NAME shapogfx_tests COMMAND shapogfx_tests)
