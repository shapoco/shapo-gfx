# The RP2350 front end of the ShapoGFX demos: ShapoGFX itself (this
# repository), the panel and touch drivers and the dual-core frame loop.
# Added by each project under example/rp2350-touch-lcd-2/ after
# pico_sdk_init() (see pico_project.cmake):
#
#   add_subdirectory(<example/rp2common> rp2common)
#   target_link_libraries(<demo> PRIVATE rp2common)
#   rp2common_setup(<demo>)
#
# Options:
#   -DRP2COMMON_INTERP=OFF       ShapoGFX's SIO interpolator paths off
#                                (SHAPOGFX2D/3D_RP2_INTERP=0), for comparison
#   -DRP2COMMON_DUAL_CORE=OFF    draw on core0 alone, for comparison
#   -DRP2COMMON_LCD_FLIP=ON      turn the screen by 180 degrees
#   -DRP2COMMON_STDIO_UART=ON    log to UART0 (GP0/GP1) instead of USB
#   -DRP2COMMON_SYS_KHZ=...      system clock (default 250000)
#   -DRP2COMMON_FLASH_CLKDIV=... flash clock = clk_sys / n (default 3)
#   -DRP2COMMON_LCD_BAUD=...     SPI clock of the panel (default 62500000)

option(RP2COMMON_INTERP "Use the SIO interpolator paths of ShapoGFX" ON)
option(RP2COMMON_DUAL_CORE "Draw every strip on both cores" ON)
option(RP2COMMON_LCD_FLIP "Turn the screen by 180 degrees" OFF)
option(RP2COMMON_STDIO_UART "Log to UART0 instead of USB" OFF)
# Empty: the defaults of clocks.hpp / runner.cpp
set(RP2COMMON_CLOCK_OPTIONS
    RP2COMMON_SYS_KHZ RP2COMMON_VREG RP2COMMON_FLASH_CLKDIV
    RP2COMMON_FLASH_RXDELAY RP2COMMON_PERI_DIV RP2COMMON_LCD_BAUD)
set(RP2COMMON_SYS_KHZ "" CACHE STRING "System clock in kHz")
set(RP2COMMON_VREG "" CACHE STRING "Core voltage (a VREG_VOLTAGE_* name)")
set(RP2COMMON_FLASH_CLKDIV "" CACHE STRING "Flash clock divider")
set(RP2COMMON_FLASH_RXDELAY "" CACHE STRING "Flash sampling delay")
set(RP2COMMON_PERI_DIV "" CACHE STRING "clk_peri = clk_sys / n (1..3)")
set(RP2COMMON_LCD_BAUD "" CACHE STRING "SPI clock of the panel in Hz")

# ShapoGFX: this repository. A project of its own would write
# add_subdirectory($ENV{SHAPOGFX_PATH} shapo-gfx).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. shapo-gfx)
# hardware_interp_headers brings PICO_RP2350 and hardware/interp.h, which turn
# the RP2 paths of the library on (the program links hardware_interp itself)
target_link_libraries(shapogfx PRIVATE hardware_interp_headers)
if(NOT RP2COMMON_INTERP)
    target_compile_definitions(shapogfx PRIVATE
        SHAPOGFX2D_RP2_INTERP=0 SHAPOGFX3D_RP2_INTERP=0)
endif()

# An INTERFACE library, as the SDK's are: its sources and the SDK's are
# compiled once, by the program
set(RP2COMMON_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/clocks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/diag.cpp
    ${CMAKE_CURRENT_LIST_DIR}/panel_st7789.cpp
    ${CMAKE_CURRENT_LIST_DIR}/touch_cst816.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runner.cpp
)
set(RP2COMMON_SOURCES ${RP2COMMON_SOURCES} PARENT_SCOPE)
add_library(rp2common INTERFACE)
target_sources(rp2common INTERFACE ${RP2COMMON_SOURCES})
target_include_directories(rp2common INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(rp2common INTERFACE
    shapoco::gfx
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_i2c
    hardware_interp
    hardware_pwm
    hardware_spi
    hardware_vreg
    hardware_watchdog
)
target_compile_definitions(rp2common INTERFACE
    RP2COMMON_DUAL_CORE=$<BOOL:${RP2COMMON_DUAL_CORE}>
    RP2COMMON_LCD_FLIP=$<BOOL:${RP2COMMON_LCD_FLIP}>
    # core1 runs on a stack of its own (runner.cpp), which leaves SCRATCH_X
    # to core0's stack
    PICO_CORE1_STACK_SIZE=0
)
foreach(opt IN LISTS RP2COMMON_CLOCK_OPTIONS)
    if(NOT "${${opt}}" STREQUAL "")
        target_compile_definitions(rp2common INTERFACE ${opt}=${${opt}})
    endif()
endforeach()

# The settings every demo executable takes
function(rp2common_setup target)
    # Warnings for the demo's own sources (not the SDK's, which the target
    # compiles as well)
    get_target_property(DEMO_SOURCES ${target} SOURCES)
    set_property(SOURCE ${DEMO_SOURCES} ${RP2COMMON_SOURCES}
        TARGET_DIRECTORY ${target}
        APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra)
    if(RP2COMMON_STDIO_UART)
        pico_enable_stdio_usb(${target} 0)
        pico_enable_stdio_uart(${target} 1)
    else()
        pico_enable_stdio_usb(${target} 1)
        pico_enable_stdio_uart(${target} 0)
    endif()
    pico_add_extra_outputs(${target})
endfunction()
