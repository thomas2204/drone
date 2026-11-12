# Combined thermal and OV2640 firmware

The pulled MLX90640 implementation is now built into the normal application.
Its sensor calibration and temperature calculations, refresh setting 0x04,
emissivity 0.95, Ta shift 8 C, iron-bow palette and bilinear 320x240 output
are retained. Automatic startup/periodic recording has been replaced by
Bluetooth `record`: both cameras prepare, share a 10-second capture window,
then finalize their files and return to idle. Existing files are not
overwritten; thermal clips use T0001.AVI, T0002.AVI, etc. See
[camera recording](camera-video.md) for commands and current validation.
The AVI format is retained from the imported code; Windows Media Player
compatibility of thermal AVI has not been established. OV2640 uses the
previously playback-confirmed H.264 MP4 format.

## Clock and peripheral configuration

The pulled clock tree is preserved: HSI 64 MHz, PLL1 M=4/N=50/P=2/Q=4/R=2,
SYSCLK 400 MHz, AHB 200 MHz, all APB buses 100 MHz. PLL2 remains
M=4/N=10/P=2/Q=2/R=2 for ADC. PLL3 remains
M=4/N=12/P=4/Q=4/R=8/FRACN=2359 for audio. I2C123 uses D2PCLK1
100 MHz. SDMMC uses PLL1Q 200 MHz with ClockDiv=4. Pins, DMA stream
assignments and DCMI active-low VSYNC are retained. UART7 RX now uses its
interrupt at priority 5, with the same entry enabled in the IOC.

Both cameras physically share I2C1 PB8/PB9: MLX90640 7-bit address 0x33,
OV2640 0x30. The thermal bus configuration 0x009034B6 (400 kHz target)
remains in i2c.c and projects.ioc. Each OV2640 SCCB transaction takes the
shared mutex, temporarily selects the proven 100 kHz timing 0x20B182BE,
and restores the previous TIMINGR before releasing it. Normal camera
startup no longer deinitializes the shared bus. Thermal reads, writes and
address probes also take the mutex. Sensor-ready polling yields with the
bus released; temperature calculations are outside bus ownership.

Two configuration exceptions are necessary to preserve working DMA:
I-cache stays enabled and D-cache disabled, matching main.c and the IOC.
The imported IOC had the opposite cache selections despite unchanged
startup code. Ordinary globals and the CPU stack stay in DTCM; only the
actual DMA buffers/file objects are placed in DMA-accessible SRAM. Moving
all globals into AXI SRAM would overflow it alongside the MP4 recorder.

## Storage and memory

All three SD users (audio, thermal, OV2640) use AppStorage rather than
remounting separate FATFS objects. Separate file objects are AXI-resident.
FatFs reentrancy remains enabled; file lock capacity is increased from 2 to
4 in both ffconf.h and the IOC. Camera completion cannot unmount an open
thermal/audio file. Thermal writes pass through an aligned AXI bounce
buffer and split at partial sector boundaries for SDMMC IDMA.

Thermal AVI renders bottom-up rows and streams them directly to the file,
preserving image content without allocating another 230400-byte framebuffer.
Failed partial frames are truncated before the AVI index is finalized.
The pulled 98304-byte RTOS heap is preserved and placed in D2 SRAM. The
16 KiB MP4 encoder task stack/control block are statically allocated in
fast DTCM. Thermal remains at the pulled Low task priority, matching the
IOC; audio remains Normal to protect refills. Video encoding and thermal
math/rendering compile with optimization even in Debug. External SDRAM is
not required by either recorder.

Thermal logs share the Bluetooth transmit mutex with existing application
and camera messages. They do not call an undefined weak __io_putchar.
`thermal_report` exposes initialization, read/clip errors, temperatures,
frame counts and the latest completed numbered clip to the debugger.

## Validation

Build the combined image with `cmake --preset Debug` and
`cmake --build build/Debug`. Release and camera-only builds are checked too.
`python tests/test_thermal_avi.py` checks the actual renderer/writer using
host file I/O: AVI size/index, orientation, aligned sector-safe writes,
existing-file protection and partial-write recovery.

Hardware results are recorded under build/thermal-merge-*-result.txt.
Both sensors have produced frames concurrently; MP4 samples passed embedded
SD readback, thermal clips closed successfully, startup audio reached
AUDIO_DONE and GPS continued receiving PVT messages. CPU clock was read as
400000000 and the restored I2C1 TIMINGR as 0x009034B6. These are functional
checks, not oscilloscope measurements of I2C rise time.

Earlier automatic-recording combined Debug hardware test: VIDEO003.MP4, 40 frames over 11639 ms,
345129 encoded sample bytes verified; T0007.AVI, 35 thermal frames,
no read/storage errors. Audio played 231075 frames; GPS PVT count reached
249. All 11 tasks remained present with 71672 RTOS heap bytes free.
Final link use: DTCM 61712/131072 bytes, AXI 496960/524288 bytes,
D2 288288/294912 bytes. AXI and D2 are near capacity; additional DMA
buffers require a memory placement review. The clock/IOC comparison is
saved in build/thermal-merge-config-audit.json.


## Thermal startup timeout recovery

A later board run reported the old generic "no ACK" message, but live
inspection found HAL_I2C_ERROR_TIMEOUT (0x20), I2C ISR BUSY (0x8000), and
PB8/PB9 both low. GPIO mode/AF/open-drain, CPU clock and TIMINGR were correct.
With the MCU halted, disabling I2C, temporarily releasing PB8/PB9 as inputs
with weak pull-ups, and restoring GPIO/CR1 made both lines high. The next
normal probe ACKed and EEPROM/calibration/frame acquisition succeeded.
Evidence: build/heat-noack-live-before.txt,
build/heat-noack-released-pins.txt and build/heat-noack-after-release.txt.
This establishes a recoverable stuck bus/controller condition; it does not
establish its original trigger or a permanent short.

Thermal startup now performs that bounded release once per failed BUSY or
HAL-timeout probe, with the shared mutex held. It waits 2 RTOS ticks with
weak pulls, restores GPIO configuration and CR1, and retries only if both
lines were released high. It does not force high outputs or change clocks,
I2C timing, filters, or device addresses. An unresponsive device or a line
still held low continues to report failure and retry later.

Probe diagnostics now distinguish HAL status, error flags, peripheral ISR
and individual SDA/SCL levels rather than labelling every error "no ACK".
thermal_report also exposes recovery count and the preceding error.
A controlled debugger injection of HAL_BUSY/TIMEOUT at the first probe
exercised the automatic recovery: recovery count 1, previous error 0x20,
released lines 3, final ACK success, and successful warmup frames. This was
a simulated probe failure, not a second naturally occurring fault.
Evidence: build/heat-recovery-injected-result.txt. TIMINGR remained
0x009034B6; GPIO mode/pulls were restored. The pinned firmware is
build/drone-heat-recovery.elf. Debug/Release builds and the three host tests
pass.
