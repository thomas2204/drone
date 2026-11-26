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

option(RC7_GUIDED_TEST "Enable the manually armed RC7 Guided test" OFF)
if(RC7_GUIDED_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/rc7_guided.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE RC7_GUIDED_TEST=1)
endif()

option(BOARD_BENCH_TEST "Run SDRAM diagnostics instead of application tasks" OFF)
option(BOARD_CAMERA_TEST "Record OV2640 H.264 MP4 to SD using internal SRAM" OFF)
option(CAMERA_RECORDING "Include MP4 recording in the normal application" ON)
option(CAMERA_FAST_MJPEG "Record direct 640x480 MJPEG AVI instead of software H.264" ON)
option(CAMERA_FLOW_BENCH "Measure image motion during MJPEG recording" OFF)
option(CAMERA_FLOW_HW_JPEG "Benchmark the STM32 JPEG peripheral for optical flow" OFF)
option(CAMERA_FLOW_MAVLINK "Publish calibrated camera flow to the FC for bench testing" OFF)
option(PMW3901_FLOW_MAVLINK "Use the SPI6 PMW3901 as the MAVLink optical flow source" OFF)
option(PMW3901_BENCH_DIAGNOSTICS "Report disarmed PMW3901 shutter and status over MAVLink" OFF)
option(PMW3901_FRAME_CAPTURE "Capture one PMW3901 grayscale frame for SWD inspection" OFF)
set(CAMERA_FLOW_FX_X100 "0" CACHE STRING "Calibrated horizontal focal length at 80x60, in hundredths of a pixel")
set(CAMERA_FLOW_FY_X100 "0" CACHE STRING "Calibrated vertical focal length at 80x60, in hundredths of a pixel")
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMERA_FLOW_BENCH=$<BOOL:${CAMERA_FLOW_BENCH}>)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMERA_FAST_MJPEG=$<BOOL:${CAMERA_FAST_MJPEG}>)
option(GPS_DIAGNOSTICS "Read GPS configuration and RF/satellite diagnostics over UART5" OFF)
set(GPS_BDS_TEST_PROFILE "0" CACHE STRING "GNSS RAM profile: 0 preserve, 1 M10 B1I, 2 M10 B1C, 3 M9 GLONASS L1")
set(GPS_MEASUREMENT_PERIOD_MS "100" CACHE STRING "GPS measurement period: 100 ms (10 Hz) or 200 ms (5 Hz)")
set_property(CACHE GPS_MEASUREMENT_PERIOD_MS PROPERTY STRINGS 100 200)
if(NOT GPS_MEASUREMENT_PERIOD_MS MATCHES "^(100|200)$")
    message(FATAL_ERROR "GPS_MEASUREMENT_PERIOD_MS must be 100 or 200")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GPS_MEASUREMENT_PERIOD_MS=${GPS_MEASUREMENT_PERIOD_MS})
set_property(CACHE GPS_BDS_TEST_PROFILE PROPERTY STRINGS 0 1 2 3)
if(NOT GPS_BDS_TEST_PROFILE MATCHES "^[0123]$")
    message(FATAL_ERROR "GPS_BDS_TEST_PROFILE must be 0, 1, 2 or 3")
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
        Core/Src/thermal_test.c Core/Src/thermal_avi.c Core/Src/thermal_palette.c
        Core/Src/record_control.c)
    set_source_files_properties(Core/Src/MLX90640_API.c Core/Src/thermal_avi.c
        Core/Src/thermal_palette.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(APP_CAMERA_ENABLED)
    if(CAMERA_FLOW_BENCH)
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/camera_flow.c)
        set_source_files_properties(Core/Src/camera_flow.c PROPERTIES COMPILE_OPTIONS "-O2")
        if(CAMERA_FLOW_HW_JPEG)
            set(STM32_JPEG_HAL_DIR "$ENV{USERPROFILE}/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver"
                CACHE PATH "Root of the CubeH7 HAL package providing the JPEG driver")
            if(NOT EXISTS "${STM32_JPEG_HAL_DIR}/Src/stm32h7xx_hal_jpeg.c")
                message(FATAL_ERROR "Set STM32_JPEG_HAL_DIR to the installed CubeH7 HAL driver directory")
            endif()
            target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/camera_flow_jpeg.c
                "${STM32_JPEG_HAL_DIR}/Src/stm32h7xx_hal_jpeg.c")
            target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${STM32_JPEG_HAL_DIR}/Inc")
            target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMERA_FLOW_HW_JPEG=1 HAL_JPEG_MODULE_ENABLED)
            set_source_files_properties(Core/Src/camera_flow_jpeg.c
                "${STM32_JPEG_HAL_DIR}/Src/stm32h7xx_hal_jpeg.c" PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endif()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Core/Src/camera_snapshot.c Core/Src/camera_jpeg.c Core/Src/camera_sccb.c
        Core/Src/camera_avi.c Core/Src/camera_bgr.c Core/Src/camera_h264.c
        Core/Src/camera_mp4.c Drivers/Camera/tjpgd/tjpgd.c)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE Drivers/Camera/ov2640 Drivers/Camera/tjpgd Drivers/Camera/minih264)
    set_source_files_properties(Core/Src/camera_h264.c PROPERTIES COMPILE_OPTIONS "-O2")
    set_source_files_properties(Core/Src/camera_snapshot.c Core/Src/camera_jpeg.c
        Core/Src/camera_avi.c Core/Src/camera_mp4.c Core/Src/camera_bgr.c
        Drivers/Camera/tjpgd/tjpgd.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CAMERA_FLOW_MAVLINK)
    if(NOT APP_CAMERA_ENABLED OR NOT CAMERA_FLOW_BENCH OR BOARD_CAMERA_TEST
        OR NOT CAMERA_FLOW_FX_X100 MATCHES "^[1-9][0-9]*$"
        OR NOT CAMERA_FLOW_FY_X100 MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Camera flow telemetry needs the application bench tracker and calibrated positive FX/FY")
    endif()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMERA_FLOW_MAVLINK=1
        CAMERA_FLOW_FX_X100=${CAMERA_FLOW_FX_X100} CAMERA_FLOW_FY_X100=${CAMERA_FLOW_FY_X100})
endif()
if(PMW3901_FLOW_MAVLINK)
    if(NOT CAMERA_FLOW_MAVLINK)
        message(FATAL_ERROR "PMW3901 telemetry uses the existing CAMERA_FLOW_MAVLINK transport and flight logger")
    endif()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/pmw3901.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PMW3901_FLOW_MAVLINK=1)
endif()
if(PMW3901_BENCH_DIAGNOSTICS)
    if(NOT PMW3901_FLOW_MAVLINK)
        message(FATAL_ERROR "PMW3901 bench diagnostics require PMW3901 flow telemetry")
    endif()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PMW3901_BENCH_DIAGNOSTICS=1)
endif()
if(PMW3901_FRAME_CAPTURE)
    if(NOT PMW3901_FLOW_MAVLINK)
        message(FATAL_ERROR "PMW3901 frame capture requires the PMW3901 driver")
    endif()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PMW3901_FRAME_CAPTURE=1)
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
    ${CMAKE_SOURCE_DIR}/Core/Src/patrol.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_raw.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_nmea.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_input.c
    ${CMAKE_SOURCE_DIR}/Core/Src/gps_ubx.c
    ${CMAKE_SOURCE_DIR}/Core/Src/led_strip.c
    ${CMAKE_SOURCE_DIR}/ws2812b/ws2812-dma/ws2812_dma.c
)

# Include MAVLink C library
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/c_library_v2
    ${CMAKE_SOURCE_DIR}/ws2812b/ws2812-dma
)
# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)


target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/ld6002.c
    Core/Src/LD6002_UART.c
    Core/Src/thermal_dma.c
)
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
