cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME projects)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
# Create firmware executable

add_executable(${CMAKE_PROJECT_NAME})

# Application-owned linker extension; CubeMX may regenerate the base script.
set(APP_DMA_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker/dma_sections.ld")
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-T${APP_DMA_LINKER_SCRIPT}")
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY
    LINK_DEPENDS "${APP_DMA_LINKER_SCRIPT}")

# ==========================================
# Custom MAVLink application
# ==========================================

# Compile MAVLink application
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Src/audio_player.c
    ${CMAKE_SOURCE_DIR}/Core/Src/mk_app.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_raw.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_nmea.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_input.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_ubx.c
    ${CMAKE_SOURCE_DIR}/Core/Src/led_strip.c
    ${CMAKE_SOURCE_DIR}/ws2812b/ws2812-dma/ws2812_dma.c
)

# Include MAVLink C library
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/MAVLink
    ${CMAKE_SOURCE_DIR}/ws2812b/ws2812-dma
)
# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Qorvo DW3000 driver and the drone's SS-TWR initiator.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/uwb_range.c
    Core/Src/uwb_protocol.c
    Core/Src/uwb_dw_port.c
    Drivers/UWB/deca_interface.c
    Drivers/UWB/deca_compat.c
    Drivers/UWB/deca_rsl.c
    Drivers/UWB/dw3000/dw3000_device.c
    Drivers/UWB/lib/qmath/src/qmath.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/UWB Drivers/UWB/dw3000 Drivers/UWB/lib/qmath/include
)
# Generated FatFs heap allocation uses pvPortMalloc/vPortFree; CMSIS-RTOS2
# does not expose their declarations. Keep this fix outside generated files.
target_compile_options(FatFs PRIVATE "$<$<COMPILE_LANGUAGE:C>:-includeFreeRTOS.h>")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
