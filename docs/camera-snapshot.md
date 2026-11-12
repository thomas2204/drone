# OV2640 camera to SD using internal SRAM

**Real camera video is now working and verified.** The dedicated Camera build
defaults to a 40-frame, approximately 10-second **H.264 MP4** clip at320x240.
Older AVI files were not playable in the user's Media Player; the user has
confirmed that the actual new MP4 recording plays correctly. See camera-video.md.
See [camera-video.md](camera-video.md) for verified results, current settings
and the repaired SDA/D0 coupling. The single-JPEG workflow below remains
available by setting `camera_snapshot_video_frames=0` before a request.

This build captures one 320x240 JPEG into a 128 KiB AXI SRAM buffer, saves a
new `0:/PHOTO000.JPG` (or the next unused number), closes it, reopens it and
compares every byte with the captured JPEG. External SDRAM is not accessed.
Existing files are never truncated. A failed write may leave a partial photo
under the newly allocated name; a failure message is not a verified image.

## Build and launch

```powershell
cmake -S . -B build/Camera -G Ninja '-DCMAKE_TOOLCHAIN_FILE=D:/projects/drone/cmake/gcc-arm-none-eabi.cmake' '-DCMAKE_BUILD_TYPE=Debug' '-DBOARD_CAMERA_TEST=ON' '-DBOARD_BENCH_TEST=OFF'
cmake --build build/Camera
```

With the board powered off, connect the OV2640 to J1 and insert the existing
FAT-formatted SD card. In VS Code Run and Debug select
**STM32Cube: Camera video to SD (internal SRAM)**. The launch entry points
directly to `build/Camera/projects.elf` and does not rebuild automatically.
Flash, then Continue (F5) when the debugger stops at main. This replaces the
SDRAM bench firmware. Normal audio, GPS, UWB and MAVLink tasks do not run in
the isolated camera test, so no task competes for the filesystem or I2C bus.

Open UART7/Bluetooth at 115200, 8N1 before starting. Expected output:

```text
CAMERA: OV2640 detected
CAMERA: complete 320x240 JPEG captured in internal SRAM
CAMERA: PASS 0:/PHOTO000.JPG (... bytes saved and verified); safe to power off
```

Only after PASS, power off and remove the card to open the JPEG on a PC.
Decoding the saved image on a PC is the final visual check. This is a single
photo test, not continuous video recording. Reset requests another photo;
while running, setting `camera_snapshot_request=1` in the debugger also requests
another attempt. Failure stages and error codes are declared in
`Core/Inc/camera_snapshot.h`; inspect the global `camera_snapshot` for details.
Report `jpeg_bytes`, `bytes_written` and `bytes_verified` must agree on success.

## Hardware and clock

The schematic identifies an OV2640 18-pin module. Reset is PG6, power-down is
PG7, SCCB uses I2C1 PB8/PB9, and data/sync use the existing DCMI pin mapping.
The driver expects the OV2640 at 7-bit SCCB address 0x30 and reads PID 0x2641
or 0x2642 before configuration. The HAL is passed the shifted address 0x60.

J1 pin 10 is unconnected in the schematic and PCB. This firmware does not
generate a camera XCLK on an unwired MCU pin. The module therefore needs its
own oscillator or a suitable external clock connection. A failed ID read can
also indicate no power, reversed connection, reset/power-down problems or an
I2C bus problem; it does not by itself prove the clock is missing.

## Data path and checks

The camera produces JPEG directly. A 128 KiB capture allocation is below the
DMA's 65535-word single-buffer limit. The driver uses snapshot mode, enables
the DCMI FRAME interrupt explicitly for a variable-length JPEG, and stops DMA
at the end of the frame. It rejects DMA/DCMI errors, buffer exhaustion and a
three-second capture timeout. It finds SOI/EOI with JPEG segment awareness and
requires a baseline 320x240 SOF before writing a file. Header checks do not
replace decoding the image. A 4 KiB aligned AXI buffer is used for SD I/O even
when SOI occurs at an unaligned capture offset. SDFile and SDFatFS remain in
their existing AXI linker section.

This firmware currently enables only I-cache. The camera test explicitly
rejects D-cache enabled rather than silently using DMA without cache handling.
DCMI DMA is changed to direct mode with single memory transfers for this test
to avoid a partially filled DMA FIFO at a short JPEG's frame boundary. The IOC
retains its ordinary DMA configuration; the override is test-local.

## Source and validation

OV2640 CIF/JPEG setup follows the Espressif driver, pinned at commit
`b3f7fe802f41f17fd7a104d517c53f1f62c8414d`:
[settings and driver](https://github.com/espressif/esp32-camera/tree/b3f7fe802f41f17fd7a104d517c53f1f62c8414d/sensors).
Only CIF initialization, CIF mode and JPEG mode tables are retained. Their
Apache-2.0 notice, and the MIT notice for OpenMV register definitions, are
included in `Drivers/Camera/ov2640`.

The isolated camera task currently overrides SCCB to conservative 100 kHz
timing 0x20B182BE for its 100 MHz I2C kernel, and sends writes as register/value
pairs, with STOP-delimited register selection/reads and 1 ms settling after
writes. The normal application's I2C1 configuration is unchanged. After the
user's SCL-short repair, PID 0x2642 was read on hardware, but configuration
still stops at register 0x2E. No hardware capture or SD-save success is claimed.

An opt-in software SCCB diagnostic is available by setting
`camera_snapshot_software_sccb=1` before requesting a capture. It disables
hardware I2C1 and uses open-drain PB8/PB9 with DWT-timed clocking, STOP-delimited
reads and observed ninth don't-care bits. It still checks that SCL can rise
and requires a valid sensor ID. This diagnostic also stopped at 0x2E with
the original table prefix, so the failure is not resolved by changing the
hardware I2C transaction implementation alone. The flag defaults to zero.

[ST AN5020](https://www.st.com/resource/en/application_note/an5020-digital-camera-interface-dcmi-on-stm32-mcus-stmicroelectronics.pdf)
describes the DCMI JPEG data path and sensor synchronization requirements.
The existing project's HAL implementation was inspected for FRAME handling
and DMA transfer limits.

Host JPEG tests use real Pillow-generated JPEGs, DMA-style padding, marker
bytes inside metadata, truncation, corrupt segment lengths and noise:

```powershell
python tests/test_camera_jpeg.py
```

The camera and SD card were not yet connected during implementation. A
successful build is not a hardware-validated capture or SD write. No camera
firmware was flashed during this preparation.

Subsequent hardware pin investigation installed this firmware with SD saving
disabled for that run. I2C SCL was found stuck low on the bare board, so no ID
or frame was obtained and no card was written. See `docs/camera-pin-check.md`.
For diagnostics only, stop at entry to `CameraSnapshot_Task`, set
`camera_snapshot_save_to_sd=0`, and continue to capture without SD access.
The flag defaults to 1 after reset.

Actual wiring clarification: the user reports two cameras sharing I2C1 on
PB8/PB9, despite the schematic's separate PH4/PH5 thermal-camera label. This
isolated test disables other application tasks but does not electrically
remove another camera from I2C1. After explicitly confirming MLX90640 removal,
the OV2640-only run still failed at register 0x2E with both hardware I2C and
software SCCB. An earlier "ready" response was misinterpreted; only the later
`camera-confirmed-ov-only-*` captures represent confirmed isolated testing.

Additional diagnostics expose before/after PCLK, HREF and VSYNC activity
counts. `camera_snapshot_skip_dsp_prefix=1` omits the reserved startup prefix
for experiments only; it defaults to zero. This experiment has not produced
a valid image and must not be used as the validated startup configuration.

Validation completed: the camera build and the regular Debug build both link;
six host JPEG tests pass. ELF symbol inspection places the 128 KiB capture
buffer at 0x24000000, the 4 KiB I/O buffer at 0x24020000 and both FatFs globals
in AXI SRAM. The linked camera image contains no SDRAM bench task, GPS task or
audio playback routine. Existing generated MAVLink headers emit packed-member
warnings; the new camera sources compile without warnings.

## Register-bank probe (2026-11-12)

The user's seller image shows an 18-pin OV2640 board with DCLK, not an
exposed XCLK. User confirms camera DCLK connects to board DCMI_PIXCLK.
Do not treat that connection as an XCLK/PCLK mismatch. An onboard oscillator
and its operation are not established by this cropped image.

Firmware `build/camera-register-probe.elf` adds the first 32 transfer records
and optional `camera_snapshot_probe_registers` bank-selection readbacks.
Default probe flag is zero so added read transactions are opt-in.
Camera build passes. Live hardware-I2C and software-SCCB runs both read PID
0x2642, read bank 1 after soft reset, and bank 0 after writing FF=00.
Writing DSP 2C=FF reports success, but the sampled bus then has SCL high and
SDA low. The next FF bank-register read fails (hardware ARLO, software
start timeout). Earlier reports naming 2E describe the next attempted write
without these diagnostic reads; they do not establish 2E itself is the cause.

Holding RESET_N low and PWDN high changes PB8/PB9 from SCL high/SDA low
to both high. This is consistent with a camera-state-dependent bus condition;
it does not prove a solder short, a bad camera, correct transport waveforms,
or a functioning oscillator. Extra probe reads can affect timing.
Results: `build/camera-register-hardware-result.txt`,
`build/camera-register-software-result.txt`,
`build/camera-register-reset-release.txt`.
No image captured and no SD operations performed. Left request/save flags
zero with camera held in reset/power-down; reset of the MCU restores default
firmware behavior. Next useful reference is the exact seller's STM32F4 driver
and module schematic, rather than another assumed module pinout.

### Reset-level verification

Retried the full original prefix with probe reads disabled, hardware I2C,
and SD saving disabled. MCU GPIOG IDR=0xA040 and ODR=0x2040 confirm PG6
RESET_N high and PG7 PWDN low after the failed initialization. These are
MCU-side digital levels, not a voltage measurement at the jumper destination.
PID remains 0x2642; trace records SDA low after 2C=FF and the subsequent
2E=DF transfer fails with ARLO. Thus the earlier held-reset cleanup state is
not why initialization fails. Result: build/camera-reset-check-result.txt.
Camera left enabled (RESET_N high/PWDN low), request flag zero, save flag
zero; no image or SD access. Debug server stopped after detach.

### Other-register write/read/restore test

Added opt-in camera_snapshot_probe_registers=2, which performs no capture or
SD operations. After normal hardware and software reset, changes only the
AGC-enable bit of bank-1 COM8 (0x13), restores it, then changes bank-0 JPEG
quantization scale QS (0x44) and restores it. Each changed and restored value
must read back exactly; PID is read again at the end. Reserved DSP 2C/2E are
not written in this diagnostic. Source register definitions: OmniVision OV2640
DS v2.2, https://dl.espressif.com/dl/schematics/Camera_OV2640.pdf.

Both STM32 hardware I2C and software SCCB passed all 19 transactions:
COM8 C7 -> C3 -> C7, QS 0C -> 14 -> 0C; PID remains 2642. No HAL errors;
both bus pins sampled high after every transaction. This establishes working
writes/readbacks for these two registers in both banks under these conditions.
It does not establish a functioning image pipeline, correct analog clock/power,
or compatibility of the reserved startup pair. Camera build passes.
Results: build/camera-other-registers-hardware-result.txt and
build/camera-other-registers-software-result.txt, using pinned
build/camera-other-registers.elf. Stage DONE with probe_registers=2 means
register diagnostic passed, not an image capture. Left camera released from
reset, request/save zero, software SCCB selected, probe mode 2 in RAM. MCU
reset restores default normal camera initialization flags. Debug server stopped.

### User-provided 480x272 reference table

Imported the user's pasted 237-entry OV2640_480x272 table without changing
its register values into Core/Inc/camera_user_table.h. Original attribution
is unknown; this is a diagnostic reference. There is an interior {00,00}
at zero-based entry 219, followed by the actual 480x272 size setup. Its
iterator uses the array length rather than the JPEG table's {00,00} sentinel.
The final bank-0 DA=09 selects RGB565 with byte swap, not JPEG; final
5A=78, 5B=44, 5C=00 selects 480x272. See OmniVision DS v2.2 table12.
A raw RGB565 frame would be 261120 bytes, exceeding the current 128KiB JPEG
buffer. These probes configure the sensor and sample activity only, without
starting DCMI DMA or accessing SD.

Added probe modes 3 (exact complete supplied table) and 4 (same table excluding
first three entries). Camera build passes. Pinned image:
build/camera-user-table-test.elf.

Mode3 hardware I2C: PID2642, 2C=FF reports success and sampled SDA low;
following 2E=DF fails ARLO, identical to original table's prefix behavior.
Mode4 hardware I2C: all remaining 234 entries complete with HAL_OK, including
the interior 00,00 and trailing size setup, but 50ms GPIO activity sampling
after 500ms settling records zero PCLK/HREF/VSYNC transitions. This proves
configuration transfer completion only, not valid image output; sampling does
not establish absence of an internal oscillator or analog signal integrity.

Results: build/camera-user-table-exact-result.txt and
build/camera-user-table-skip-result.txt. No image captured or SD access.
Left request/save zero, probe mode4, hardware I2C, RESET_N high/PWDN low.
MCU reset restores default camera flags. Debug server stopped after detach.
