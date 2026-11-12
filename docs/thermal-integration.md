# Thermal camera configuration and task scaffold

The reference is `H743_DEV_BOARD_TEST.ioc` supplied on 2026-11-12.
Only its thermal I2C configuration and relevant FreeRTOS settings are applied.
The reference does not contain the sensor driver or acquisition code.

## Clock and I2C

Actual wiring clarified on 2026-11-12: the user's MLX90640 and OV2640 both
share I2C1 PB8/PB9. The schematic's PH4/PH5 heat-camera test-point labels refer
to I2C2, but are not the actual connection reported by the user. Keep this
thermal scaffold on `hi2c1` and the shared I2C1 mutex. MLX90640 normally uses
7-bit address 0x33; OV2640 uses 0x30. Confirm the actual addresses when both
are connected. The thermal acquisition driver remains unimplemented.

`projects.ioc` now describes the existing firmware clock configuration:
HSI 64 MHz, PLL1 M=4 / N=50 / P=2 / Q=4 / R=2, wide VCO,
SYSCLK 400 MHz, AHB 200 MHz, and all APB buses 100 MHz.
The reference instead uses HSE with M=5 / N=160. Keeping HSI avoids changing
the shared PLL input used by this project's ADC and audio PLLs. PLL2, PLL3,
USB HSI48, pin assignments, and peripheral selections are preserved.

I2C1 on PB8/PB9 uses the reference's I2C3 Fast-mode timing `0x009034B6`
with a 100 MHz peripheral kernel clock. The source already had this value;
the CubeMX file was stale. The reference's default rise/fall inputs replace
the old I2C1 Standard-mode overrides. Confirm the actual bus waveform on
hardware, including external pull-ups and the added module's capacitance.
I2C2 and I2C3 timing entries are synchronized to their existing source
values for the same 100 MHz kernel clock; neither bus changes in firmware.

## FreeRTOS

- Existing tasks are retained. `thermalTask` calls `StartThermalTask`, at
  AboveNormal priority with 2048 words (8192 bytes) of stack, matching the
  thermal task allocation in the reference. It currently only sleeps.
- FreeRTOS heap is 65536 bytes. Stack overflow checking (mode 2) and allocation
  failure hooks are enabled as in the reference. Per-task newlib reentrancy
  remains disabled, preserving the original project's C-library configuration;
  the thermal task and I2C mutex do not require it.
  The failure hooks stop through `Error_Handler` without logging or allocation.
- `i2c1MutexHandle`, declared in `i2c.h`, is created before the scheduler starts.
  All future task-level users of `hi2c1` must use it; declaring a mutex alone
  does not protect drivers that access the handle directly.

For a blocking HAL transaction, acquire the mutex, check acquisition succeeded,
perform the complete register-read/write sequence, then release on every exit
path. For DMA/interrupt transfers, the owning task must wait for completion
before releasing it; an ISR can signal completion but cannot release a mutex.
Keep frame-ready polling delays, temperature calculations, display and filesystem
work outside bus ownership. A CMSIS mutex timeout is in RTOS ticks.

No display or logger tasks, USART configuration, JPEG support, or FatFs options
are copied. The existing SD/audio filesystem setup stays in place.

## Remaining integration and hardware checks

Provide the working thermal project's sensor driver and acquisition functions
to replace the placeholder in `StartThermalTask`. Its platform I2C layer must
use `hi2c1` and the shared mutex, without reinitializing I2C or system clocks.
Check whether the driver expects a 7-bit address or HAL's shifted address.

Before using the camera, confirm all devices on I2C1 support 400 kHz, have
distinct addresses, and use compatible bus voltage levels. Measure SCL and
rise time, then verify frame acquisition while GPS, MAVLink, UWB, LEDs and
audio run. The source already ran at 400 MHz, so this update does not establish
that SDRAM timings or other existing peripheral settings have been hardware
validated. No firmware has been flashed as part of this change.

## Validation

`cmake --build --preset Debug` succeeds. The enlarged FreeRTOS heap fits in
the 128 KB DTCM region. This confirms link-time capacity, not runtime stack
headroom.

CubeMX 6.18.1 loaded and saved an isolated copy, retaining the clock, I2C,
thermal task and mutex entries. Full generation did not finish: CubeMX
reported `IP not ready for code generation: FATFS`. The validation copy and
logs are under `build/thermal-cubemx-check`; generated output was not applied
to the working project. Review the FatFs configuration in CubeMX before
regenerating the complete project. No manual regeneration is needed to build
the source changes delivered here.
