cmake_minimum_required(VERSION 3.13)
project(shapogfx LANGUAGES CXX)

# Perspective-correct texture mapping (adds a per-pixel divide; off = affine)
option(SHAPOGFX3D_CORRECT_PERSPECTIVE "Enable perspective-correct texture mapping" OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SHAPOGFX_TOP_LEVEL ON)
else()
    set(SHAPOGFX_TOP_LEVEL OFF)
endif()
option(SHAPOGFX_BUILD_EXAMPLES "Build the example programs" ${SHAPOGFX_TOP_LEVEL})

add_library(shapogfx STATIC
    src/gfx3d/gfx3d.cpp
)
add_library(shapoco::gfx ALIAS shapogfx)
target_include_directories(shapogfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(shapogfx PUBLIC cxx_std_17)
if(SHAPOGFX3D_CORRECT_PERSPECTIVE)
    target_compile_definitions(shapogfx PRIVATE SHAPOGFX3D_CORRECT_PERSPECTIVE=1)
endif()

if(SHAPOGFX_BUILD_EXAMPLES AND NOT EMSCRIPTEN)
    add_subdirectory(example/demo3d)
endif()
