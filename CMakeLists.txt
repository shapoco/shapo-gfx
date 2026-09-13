cmake_minimum_required(VERSION 3.13)
project(shapogfx LANGUAGES CXX)

# Perspective correction of texture coordinates: 0 = off (affine),
# 1 = vertical only (two divides per span), 2 = full (per-pixel divide).
# Leave empty to use the library default (1).
set(SHAPOGFX3D_CORRECT_PERSPECTIVE "" CACHE STRING
    "Perspective correction level: 0, 1 or 2 (empty = library default)")
set_property(CACHE SHAPOGFX3D_CORRECT_PERSPECTIVE PROPERTY STRINGS "" 0 1 2)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SHAPOGFX_TOP_LEVEL ON)
else()
    set(SHAPOGFX_TOP_LEVEL OFF)
endif()
option(SHAPOGFX_BUILD_EXAMPLES "Build the example programs" ${SHAPOGFX_TOP_LEVEL})

file(GLOB SHAPOGFX_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx2d/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx3d/*.cpp
)
add_library(shapogfx STATIC ${SHAPOGFX_SOURCES})
add_library(shapoco::gfx ALIAS shapogfx)
target_include_directories(shapogfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(shapogfx PUBLIC cxx_std_17)
if(NOT SHAPOGFX3D_CORRECT_PERSPECTIVE STREQUAL "")
    target_compile_definitions(shapogfx PRIVATE
        SHAPOGFX3D_CORRECT_PERSPECTIVE=${SHAPOGFX3D_CORRECT_PERSPECTIVE})
endif()

if(SHAPOGFX_BUILD_EXAMPLES AND NOT EMSCRIPTEN)
    add_subdirectory(example/wasm/demo2d)
    add_subdirectory(example/wasm/demo3d)
endif()
