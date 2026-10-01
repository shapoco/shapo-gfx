# ShapoGFX as an ESP-IDF component, included by the CMakeLists.txt of the
# repository when ESP-IDF is the one reading it. The options come from Kconfig
# (idf.py menuconfig, or CONFIG_SHAPOGFX... in sdkconfig.defaults).
set(SHAPOGFX_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

file(GLOB SHAPOGFX_SOURCES
    ${SHAPOGFX_ROOT}/src/gfx2d/*.cpp
    ${SHAPOGFX_ROOT}/src/gfx3d/*.cpp
)

idf_component_register(
    SRCS ${SHAPOGFX_SOURCES}
    INCLUDE_DIRS "${SHAPOGFX_ROOT}/include"
)

# Read by config.hpp, so they have to be the same in every translation unit:
# passed on to the components that require this one.
set(SHAPOGFX_PUBLIC_BOOLS
    SHAPOGFX_FORMAT_GRAY1 SHAPOGFX_FORMAT_RGB444 SHAPOGFX_FORMAT_ARGB4444
    SHAPOGFX_FORMAT_RGB565_SWAPPED SHAPOGFX_FORMAT_RGB565)
set(SHAPOGFX_PUBLIC_INTS SHAPOGFX_COORD_BITS)
# Read by src/gfx2d/*.cpp and src/gfx3d/*.cpp only
set(SHAPOGFX_PRIVATE_BOOLS
    SHAPOGFX2D_TRANSFORM SHAPOGFX2D_BLEND SHAPOGFX2D_COLOR_KEY SHAPOGFX2D_RIG
    SHAPOGFX2D_ANTIALIAS
    SHAPOGFX3D_TEXTURE SHAPOGFX3D_GOURAUD SHAPOGFX3D_BLEND
    SHAPOGFX3D_LINES SHAPOGFX3D_POINTS SHAPOGFX3D_FIXED_POINT)
set(SHAPOGFX_PRIVATE_INTS
    SHAPOGFX2D_STACK_DEPTH
    SHAPOGFX3D_CORRECT_PERSPECTIVE SHAPOGFX3D_PERSPECTIVE_STEP
    SHAPOGFX3D_GOURAUD_STEP
    SHAPOGFX3D_STACK_DEPTH SHAPOGFX3D_VCACHE_SIZE SHAPOGFX3D_LAYER_MAX)

foreach(scope PUBLIC PRIVATE)
    # A bool that is off is not set at all
    foreach(opt IN LISTS SHAPOGFX_${scope}_BOOLS)
        if(CONFIG_${opt})
            target_compile_definitions(${COMPONENT_LIB} ${scope} ${opt}=1)
        else()
            target_compile_definitions(${COMPONENT_LIB} ${scope} ${opt}=0)
        endif()
    endforeach()
    foreach(opt IN LISTS SHAPOGFX_${scope}_INTS)
        target_compile_definitions(${COMPONENT_LIB} ${scope}
            ${opt}=${CONFIG_${opt}})
    endforeach()
endforeach()

if(CONFIG_SHAPOGFX3D_DEPTH_BITS_16)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHAPOGFX3D_DEPTH_BITS=16)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHAPOGFX3D_DEPTH_BITS=32)
endif()

# Left undefined, the library detects the FPU from the target
if(CONFIG_SHAPOGFX2D_FPU_SQRT_ON)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHAPOGFX2D_FPU_SQRT=1)
elseif(CONFIG_SHAPOGFX2D_FPU_SQRT_OFF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHAPOGFX2D_FPU_SQRT=0)
endif()
