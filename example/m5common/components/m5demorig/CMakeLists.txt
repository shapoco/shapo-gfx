# The M5Stack front end of demorig: the frame loop, the touch input and the
# panel output (SPI on the CoreS3, PPA + MIPI-DSI on the Tab5).
if(IDF_TARGET STREQUAL "esp32p4")
    set(PANEL_SRC "panel_ppa.cpp")
    set(PANEL_REQUIRES esp_driver_ppa esp_mm)
else()
    set(PANEL_SRC "panel_spi.cpp")
    set(PANEL_REQUIRES "")
endif()

idf_component_register(
    SRCS "runner.cpp" "${PANEL_SRC}"
    INCLUDE_DIRS "."
    REQUIRES demorig m5unified m5gfx esp_timer ${PANEL_REQUIRES}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# idf.py -DM5DEMORIG_DUAL_CORE=0 build: draw on core0 alone, for comparison
if(DEFINED M5DEMORIG_DUAL_CORE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        M5DEMORIG_DUAL_CORE=${M5DEMORIG_DUAL_CORE})
endif()
