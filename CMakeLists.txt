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

option(BOARD_BENCH_TEST "Run SDRAM diagnostics instead of application tasks" OFF)
option(BOARD_CAMERA_TEST "Record OV2640 H.264 MP4 to SD using internal SRAM" OFF)
option(CAMERA_RECORDING "Include MP4 recording in the normal application" ON)
option(GPS_DIAGNOSTICS "Read GPS configuration and RF/satellite diagnostics over UART5" OFF)
set(GPS_BDS_TEST_PROFILE "0" CACHE STRING "BDS RAM test: 0 preserve, 1 B1I, 2 B1C")
set(GPS_MEASUREMENT_PERIOD_MS "100" CACHE STRING "GPS measurement period: 100 ms (10 Hz) or 200 ms (5 Hz)")
set_property(CACHE GPS_MEASUREMENT_PERIOD_MS PROPERTY STRINGS 100 200)
if(NOT GPS_MEASUREMENT_PERIOD_MS MATCHES "^(100|200)$")
    message(FATAL_ERROR "GPS_MEASUREMENT_PERIOD_MS must be 100 or 200")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GPS_MEASUREMENT_PERIOD_MS=${GPS_MEASUREMENT_PERIOD_MS})
set_property(CACHE GPS_BDS_TEST_PROFILE PROPERTY STRINGS 0 1 2)
if(NOT GPS_BDS_TEST_PROFILE MATCHES "^[012]$")
    message(FATAL_ERROR "GPS_BDS_TEST_PROFILE must be 0, 1 or 2")
endif()
if(NOT GPS_BDS_TEST_PROFILE STREQUAL "0" AND NOT GPS_DIAGNOSTICS)
    message(FATAL_ERROR "BDS comparison profiles require GPS_DIAGNOSTICS=ON for ACK/readback")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GPS_BDS_TEST_PROFILE=${GPS_BDS_TEST_PROFILE})
if(GPS_DIAGNOSTICS)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GPS_DIAGNOSTICS_ENABLED=1)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/gps_diag.c)
endif()
if(BOARD_BENCH_TEST AND BOARD_CAMERA_TEST)
    message(FATAL_ERROR "Select either SDRAM bench or camera test, not both")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    BOARD_BENCH_TEST=$<BOOL:${BOARD_BENCH_TEST}>
    BOARD_CAMERA_TEST=$<BOOL:${BOARD_CAMERA_TEST}>)
set(APP_CAMERA_ENABLED OFF)
if(BOARD_CAMERA_TEST OR (CAMERA_RECORDING AND NOT BOARD_BENCH_TEST))
    set(APP_CAMERA_ENABLED ON)
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE APP_CAMERA_ENABLED=$<BOOL:${APP_CAMERA_ENABLED}>)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/app_storage.c Core/Src/app_log.c)
if(NOT BOARD_CAMERA_TEST AND NOT BOARD_BENCH_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Core/Src/MLX90640_API.c Core/Src/MLX90640_I2C_Driver.c
        Core/Src/thermal_test.c Core/Src/thermal_avi.c Core/Src/thermal_palette.c)
    set_source_files_properties(Core/Src/MLX90640_API.c Core/Src/thermal_avi.c
        Core/Src/thermal_palette.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(APP_CAMERA_ENABLED)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Core/Src/camera_snapshot.c Core/Src/camera_jpeg.c Core/Src/camera_sccb.c
        Core/Src/camera_avi.c Core/Src/camera_bgr.c Core/Src/camera_h264.c
        Core/Src/camera_mp4.c Drivers/Camera/tjpgd/tjpgd.c)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE Drivers/Camera/ov2640 Drivers/Camera/tjpgd Drivers/Camera/minih264)
    set_source_files_properties(Core/Src/camera_h264.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(BOARD_BENCH_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/board_bench.c)
    # Retain the FatFs globals checked by the shared DMA linker script even
    # though the SDRAM-only task never opens the SD card.
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        "-Wl,--undefined=SDFatFS,--undefined=SDFile")
endif()

# Application-owned linker extension; CubeMX may regenerate the base script.
set(APP_DMA_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker/dma_sections.ld")
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-T${APP_DMA_LINKER_SCRIPT}")
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY
    LINK_DEPENDS "${APP_DMA_LINKER_SCRIPT}" "${CMAKE_SOURCE_DIR}/STM32H743xx_FLASH.ld")

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
