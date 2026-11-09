# Self-checking tests. Build with sanitizers for the strictest run, e.g.
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
#   cmake --build build && ctest --test-dir build --output-on-failure
# and with -fsanitize=thread in place of address,undefined for the tests that
# draw on two threads.
# Adding -DSHAPOGFX_ARCH_WORD_LOADS=1 to CMAKE_CXX_FLAGS runs the same tests
# over the word loads of the ESP8266 (src/gfx2d/arch.hpp), with or without the
# sanitizers.
add_executable(shapogfx_tests pixel_test.cpp graphics2d_test.cpp gfx3d_test.cpp tools_test.cpp
                              rig_test.cpp vg_test.cpp fixed_test.cpp test_main.cpp)
target_include_directories(shapogfx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# testVersion() reads library.json to check it matches version.hpp
target_compile_definitions(shapogfx_tests PRIVATE
    SHAPOGFX_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
# testRenderContexts() and testRigThreads() draw on two threads
find_package(Threads REQUIRED)
target_link_libraries(shapogfx_tests PRIVATE shapoco::gfx Threads::Threads)
target_compile_options(shapogfx_tests PRIVATE -Wall -Wextra)
# The renderer options change no public type, but the tests need to know which
# features are compiled in so that they can skip what cannot be drawn.
if(SHAPOGFX_FEATURE_DEFS)
    target_compile_definitions(shapogfx_tests PRIVATE ${SHAPOGFX_FEATURE_DEFS})
endif()
add_test(NAME shapogfx_tests COMMAND shapogfx_tests)
