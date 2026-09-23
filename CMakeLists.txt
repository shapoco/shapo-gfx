cmake_minimum_required(VERSION 3.13)
project(shapogfx LANGUAGES CXX)

# Options of the 3D renderer. Each is read by src/gfx3d/*.cpp only and changes
# no public type, so leaving one empty simply keeps the library default.
# Turning a feature off removes its code and shrinks the per-triangle and
# per-span working memory, so the same arena holds more geometry.
set(SHAPOGFX3D_CORRECT_PERSPECTIVE "" CACHE STRING
    "Perspective correction level: 0, 1 or 2 (empty = library default)")
set_property(CACHE SHAPOGFX3D_CORRECT_PERSPECTIVE PROPERTY STRINGS "" 0 1 2)
set(SHAPOGFX3D_PERSPECTIVE_STEP "" CACHE STRING
    "Level 2: pixels between exact texture coordinates (power of two)")
set(SHAPOGFX3D_GOURAUD_STEP "" CACHE STRING
    "Pixels between updates of the color modulating a texture (power of two)")
set(SHAPOGFX3D_TEXTURE "" CACHE STRING "Texture and environment mapping (0/1)")
set_property(CACHE SHAPOGFX3D_TEXTURE PROPERTY STRINGS "" 0 1)
set(SHAPOGFX3D_GOURAUD "" CACHE STRING
    "Gouraud shading; 0 selects flat shading (0/1)")
set_property(CACHE SHAPOGFX3D_GOURAUD PROPERTY STRINGS "" 0 1)
set(SHAPOGFX3D_BLEND "" CACHE STRING "Translucency (0/1)")
set_property(CACHE SHAPOGFX3D_BLEND PROPERTY STRINGS "" 0 1)
set(SHAPOGFX3D_LINES "" CACHE STRING "LINES / LINE_STRIP / LINE_LOOP (0/1)")
set_property(CACHE SHAPOGFX3D_LINES PROPERTY STRINGS "" 0 1)
set(SHAPOGFX3D_POINTS "" CACHE STRING "POINTS primitives (0/1)")
set_property(CACHE SHAPOGFX3D_POINTS PROPERTY STRINGS "" 0 1)
set(SHAPOGFX3D_STACK_DEPTH "" CACHE STRING "Levels of the matrix stack")
set(SHAPOGFX3D_VCACHE_SIZE "" CACHE STRING
    "Entries of the vertex cache (power of two)")
set(SHAPOGFX3D_LAYER_MAX "" CACHE STRING "Layers a scene can hold (1..128)")
set(SHAPOGFX3D_FIXED_POINT "" CACHE STRING
    "Fixed-point vertex stage and setup, for cores without an FPU (0/1)")
set_property(CACHE SHAPOGFX3D_FIXED_POINT PROPERTY STRINGS "" 0 1)

set(SHAPOGFX3D_DEPTH_BITS "" CACHE STRING
    "Depth resolution of the span records: 32 or 16 bits")
set_property(CACHE SHAPOGFX3D_DEPTH_BITS PROPERTY STRINGS "" 32 16)

# Shared by gfx2d and gfx3d and read by config.hpp, so it has to be the same
# in every translation unit: it is passed on to the users of the library.
set(SHAPOGFX_COORD_BITS "" CACHE STRING
    "Bits of a screen coordinate and of a surface size (1..15, default 11)")

set(SHAPOGFX3D_OPTIONS
    SHAPOGFX3D_CORRECT_PERSPECTIVE SHAPOGFX3D_PERSPECTIVE_STEP
    SHAPOGFX3D_GOURAUD_STEP
    SHAPOGFX3D_TEXTURE SHAPOGFX3D_GOURAUD SHAPOGFX3D_BLEND
    SHAPOGFX3D_LINES SHAPOGFX3D_POINTS
    SHAPOGFX3D_STACK_DEPTH SHAPOGFX3D_VCACHE_SIZE SHAPOGFX3D_LAYER_MAX
    SHAPOGFX3D_FIXED_POINT SHAPOGFX3D_DEPTH_BITS)
set(SHAPOGFX_FEATURE_DEFS "")
foreach(opt IN LISTS SHAPOGFX3D_OPTIONS)
    if(NOT "${${opt}}" STREQUAL "")
        list(APPEND SHAPOGFX_FEATURE_DEFS ${opt}=${${opt}})
    endif()
endforeach()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SHAPOGFX_TOP_LEVEL ON)
else()
    set(SHAPOGFX_TOP_LEVEL OFF)
endif()
option(SHAPOGFX_BUILD_EXAMPLES "Build the example programs" ${SHAPOGFX_TOP_LEVEL})
option(SHAPOGFX_BUILD_TESTS "Build the tests (run with ctest)" ${SHAPOGFX_TOP_LEVEL})

file(GLOB SHAPOGFX_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx2d/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx3d/*.cpp
)
add_library(shapogfx STATIC ${SHAPOGFX_SOURCES})
add_library(shapoco::gfx ALIAS shapogfx)
target_include_directories(shapogfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(shapogfx PUBLIC cxx_std_17)
if(SHAPOGFX_FEATURE_DEFS)
    target_compile_definitions(shapogfx PRIVATE ${SHAPOGFX_FEATURE_DEFS})
endif()
if(NOT "${SHAPOGFX_COORD_BITS}" STREQUAL "")
    target_compile_definitions(shapogfx PUBLIC
        SHAPOGFX_COORD_BITS=${SHAPOGFX_COORD_BITS})
endif()

if(SHAPOGFX_BUILD_EXAMPLES AND NOT EMSCRIPTEN)
    add_subdirectory(example/wasm/demo2d)
    add_subdirectory(example/wasm/demo3d)
endif()

if(SHAPOGFX_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(test)
endif()
