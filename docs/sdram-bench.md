# SDRAM bench test

The schematic and PCB at `D:/pcbs/dronetestv1/test.kicad_sch` and
`test.kicad_pcb` identify U2 as W9825G6KH-6, connected to FMC SDRAM bank 1.
The existing SD card/audio read path is already hardware-confirmed by the user.
This diagnostic tests SDRAM only; it does not initialize, read or write the card.

## Build and run

The diagnostic is opt-in and uses a separate build directory:

```powershell
cmake -S . -B build/Bench -G Ninja '-DCMAKE_TOOLCHAIN_FILE=D:/projects/drone/cmake/gcc-arm-none-eabi.cmake' '-DCMAKE_BUILD_TYPE=Debug' '-DBOARD_BENCH_TEST=ON'
cmake --build build/Bench
```

The resulting firmware is `build/Bench/projects.elf`. In VS Code's Run and
Debug selector choose **STM32Cube: SDRAM bench test**. This launch entry points
directly to the bench ELF; the original launch entry automatically chooses the
active application build and can therefore flash the regular GPS/UWB firmware.
The bench entry does not rebuild automatically; run the build command above
after changing test code. Once the debugger stops at `main`, press Continue
(F5) to let the test run. Connect the existing UART7/Bluetooth terminal at
115200 baud, 8N1 before resetting the board. Normal application tasks, including
audio, GPS, MAVLink, UWB and camera acquisition, do not run in this build.
The board does not need an SD card installed for the memory test.

Expected output is six repetitions of `SDRAM: pass complete`, followed by
`SDRAM: PASS` and a repeated `BENCH: PASS; safe to power off` message.
Allow time for six complete writes and reads of the 32 MiB region in the Debug
build. It has not passed on hardware merely because the firmware compiles.

Inspect the global `board_bench` in the debugger:

- `sdram_result`: 0 pending, 1 passed, -1 failed.
- `stage`: 1 startup, 2 memory access, 3 finished successfully.
- `memory_pass`: 1 zero, 2 all ones, 3 alternating A, 4 alternating 5,
  5 word index, 6 complemented word index.
- `address`, `expected`, `actual`: first comparison failure.
- `hal_status`, `fmc_clock_hz`: startup diagnostics.
- `sdram_ms`: elapsed initialization and test time.
- `tested_bytes`: actual region size covered by this run. A PASS applies only
  to this region, not automatically to the whole physical chip.
- `manual_a11`: 0 for ordinary FMC addressing; 1 for the software-controlled
  A11 diagnostic described below. A manual-mode PASS does not validate ordinary
  memory-mapped addressing.

The debugger-editable `board_bench_test_bytes` defaults to 33554432. Stop at
the entry to `TestSDRAM`, set it to 2097152, then continue to test just the
first 2 MiB. The value is captured once at entry; do not change it mid-test.
Reset restores the 32 MiB default, so a limited-region PASS is not a permanent
application workaround or a change to the chip's configured geometry.

If external memory access faults, the task may stop before setting a failure
result; inspect the fault registers and the report stage in the debugger.
The test overwrites the entire external SDRAM. No application data is stored
there by this diagnostic. It does not test camera DMA or measure video throughput.

## Initialization and coverage

FMC is 200 MHz and SDCLK is divided by two, giving a 10 ns SDRAM clock period.
The test rejects a different FMC frequency. Controller timing in both the IOC
and generated FMC source is now 2/8/5/7/3/2/2 cycles for
TMRD/TXSR/TRAS/TRC/TWR/TRP/TRCD. CAS latency remains 2. The longer TWR also
satisfies the FMC relationships with TRAS, TRC, TRCD and TRP.

The frequency check reads the FMC source selector and queries HCLK when
Domain 1 HCLK is selected. This project's HAL does not support FMC in
`HAL_RCCEx_GetPeriphCLKFreq`; passing FMC to that API returns zero and caused
the initial diagnostic build to fail before touching memory.

The bench startup enables the clock, waits at least 200 us, precharges all
banks, issues eight auto-refresh cycles and programs mode register 0x220
(burst length 1, sequential, CAS 2, single writes). Refresh count is
`floor(100000000 * 0.064 / 8192) - 20 = 761`.

These settings use Winbond's W9825G6KH datasheet, sections 7.1, 7.2 and 9.5:
[manufacturer-authored datasheet](https://evelta.com/content/datasheets/153-W9825G6KH-6.pdf).
The -6 grade specifies tXSR 72 ns, tRAS 42 ns, tRC 60 ns, tRP/tRCD 15 ns,
and 8192 refresh cycles within 64 ms. The values are for this board's clock;
recalculate them if the FMC clock changes.

Only the bench build adds a 32 MiB, non-executable, noncacheable Normal-memory
MPU region at 0xC0000000, overriding the existing default no-access region.
The diagnostic uses volatile memory accesses, walking ones/zeros on the 16-bit
bus, independent byte writes for the two DQM lanes, and six full-region
write/read passes. Each pass waits at least 100 ms before verification to
exercise refresh; index patterns check address aliasing across rows and banks.

Regular Debug/Release builds leave the diagnostic disabled. The corrected FMC
timing is shared, but SDRAM chip startup and MPU access remain bench-only until
hardware validation. Rebuild and flash the regular firmware to restore the
application after testing. CubeMX regeneration can replace the bench conditional
around `MX_SDMMC1_SD_Init()` in main.c; review that hook after regeneration.

## Next step

Once SDRAM passes, integrate OV2640 initialization and capture one frame into
SDRAM, then save it through the existing FatFs path. A saved, readable image
will check the newly exercised SD write path. Continuous recording comes after
single-frame capture works. Camera connector pin 10 is unconnected in the
schematic and PCB, so the module's clock source must be established first.

## ST-Link investigation, 2026-11-12

Connected through the user's existing ST-LINK GDB server at localhost:61234.
The original stage-1 failure was a diagnostic software bug: the unsupported
FMC frequency API returned zero. Corrected it and flashed the bench firmware.

At the normal 100 MHz SDRAM clock, walking data bits, DQM byte writes and four
full 32 MiB constant-pattern passes completed. The fifth pass, word-index data,
failed with stage=2, address=0xC0000000, expected=0x00000000,
actual=0x00080000. A direct debugger write/read test confirmed that
0xC0000000 and 0xC0200000 alias. Individually testing offsets 1<<2 through
1<<24 found the alias only for address bit 21.

Temporarily reducing SDCLK to FMC/3 (about 66.7 MHz), with refresh count 500,
reproduced the same fifth-pass failure. Explicit precharge-all commands between
accesses did not eliminate the alias. Neither did changing PG1 output speed
through all four settings. These temporary debugger settings are not source
changes and are cleared by reset.

With this 16-bit, 9-column-bit mapping, CPU address bit 21 selects row bit 11.
The schematic net is FMC_A11: MCU U1 PG1/pin 67 to SDRAM U2 A11/pin 35.
The GPIO registers show PG1 in AF12, matching the STM32H743 datasheet.
Manually controlling PG1 as GPIO produced correct low/high input readback and
two distinct memory values after precharging between changes. Therefore these
tests do not prove an open trace or a damaged SDRAM chip. The exact electrical
or controller-level cause of automatic address aliasing remains unresolved.

The next physical check is the A11 waveform at both chip pins during alternate
accesses to 0xC0000000 and 0xC0200000, alongside SDCLK and SDNRAS. Inspect the
A11 solder joints and check continuity with power removed. A static GPIO test
cannot validate the address waveform at the SDRAM sampling edge.

References:
[ST RM0433, SDRAM address mapping](https://www.st.com/resource/en/reference_manual/rm0433-stm32h743-753-and-stm32h750-value-line-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf),
[STM32H743 datasheet, PG1 alternate functions](https://www.st.com/resource/en/datasheet/stm32h743ig.pdf).

### Follow-up configuration review

Rechecked Winbond's block diagram: each internal bank is 8192 rows x 512
columns x 16 bits. Thus 13 row bits, 9 column bits, four banks and a 16-bit
bus agree with the configured 32 MiB geometry. The mode register and FMC
controller both select CAS 2. The measured 200 MHz FMC kernel and divide-by-two
SDCLK agree with the timing calculation. No geometry mismatch was found.

The PCB's A11 net has five track segments and one via, approximately 17.76 mm
of track. Its endpoints match U1 pad 67 and U2 pad 35 after footprint transforms;
the segment endpoints join through the via. This verifies the design's routed
connection, not the manufactured board's continuity or signal integrity.

One remaining software candidate is I/O compensation. ST's official H743
SDRAM example enables CSI, enables SYSCFG and calls
`HAL_EnableCompensationCell()`; the current project's clock initialization
does not. This difference has not yet been tested on the user's board and
must not be reported as the established cause of the A11 alias.

[ST's H743 SDRAM example](https://github.com/STMicroelectronics/STM32CubeH7/blob/master/Projects/STM32H743I-EVAL/Examples/FMC/FMC_SDRAM/Src/main.c)

The probe was initially unavailable, then the user reconnected it. A temporary
ST-LINK GDB server on port 61244 was used for the following live checks:

- Enabling CSI and I/O compensation produced READY=1 (CCCSR=0x101), but the
  two-address alias persisted.
- All twelve combinations of CAS 2/3, read-pipe delay 0/1/2 and read burst
  disabled/enabled reproduced the alias. Both controller CAS and the SDRAM
  mode register were changed together, with precharge-all before each change.
- Reset restored the source configuration before the limited-region test.
- The first 2 MiB (0xC0000000 through 0xC01FFFFF) passed all six full-region
  write/read passes, walking data bits and DQM byte checks at 100 MHz SDCLK.
  The report was stage=3, sdram_result=1, memory_pass=6,
  tested_bytes=2097152, fmc_clock_hz=200000000, sdram_ms=2003.

The configuration review did not identify a correction for the full 32 MiB.
The lower 2 MiB is a hardware-tested candidate for a restricted camera buffer
allocation, pending integration and camera-DMA testing. The whole 32 MiB must
not be treated as validated. No solder short has been established.

### Full-capacity restoration and pin-coupling tests

The next investigation changed NC from 9 to 8 temporarily. The missing CPU
address bit moved from 21 to 20, as expected for SDRAM row bit 11 (A11), rather
than staying at a fixed CPU address. Restored NC=9 after the check.

Tested 31 memory-interface GPIO signals: all 13 address lines, both bank-address
lines and all 16 data lines. First placed the SDRAM in self-refresh, disabled
the FMC controller and held the memory deselected with its clock low. All
tested pins were inputs with pull-ups, then individually driven low using
open-drain mode. Each selected pin alone read low; no other tested pin followed
it. All tested inputs also followed pull-down bias. This found no hard-short or
stuck-level indication among those 31 signals, including PG1 and the suspected
neighbouring address/data nets. It does not establish resistance values or
prove the absence of every possible board fault. Reset restored peripheral
configuration after this test.

Added an opt-in diagnostic flag, `board_bench_manual_a11`, default zero. Stop
at entry to `BoardBench_Task`, set this flag to 1, leave
`board_bench_test_bytes=33554432`, and continue. In this mode PG1 is a GPIO
output while the test runs. Before crossing each 2 MiB boundary, the diagnostic
drains outstanding writes with DSB, precharges all banks, sets PG1 according
to byte-address bit 21 and allows one RTOS tick of settling time. The original
AF12 configuration is restored on completion, including failure exits.

An initial prototype lacked the boundary ordering/settling guard and failed;
the corrected diagnostic passed all six full 32 MiB write/read passes at
100 MHz SDCLK, plus the walking data-bit and byte-lane tests. The report was:

```text
stage=3 sdram_result=1 memory_pass=6 tested_bytes=33554432
manual_a11=1 fmc_clock_hz=200000000 sdram_ms=24122
```

This establishes that all 32 MiB passed this diagnostic when A11 was held in
software for each block. It does not repair transparent FMC addressing.
After restoring PG1 to AF12, the two-address test still aliased at offsets
0 and 0x200000. Thus normal automatic addressing remains faulty.

The proven software-controlled access is suitable for investigating a future
block-based driver, not for exposing the whole chip as unrestricted memory.
Any such driver must serialize access, select the correct A11 level before
access, handle open-row state and prevent DMA or another bus master from
accessing a different block during selection. The current firmware implements
the diagnostic only, not that application driver. Reset restores the ordinary
32 MiB test (manual flag zero).

Captured evidence is in `build/sdram-column-map-results.txt`,
`build/sdram-pin-coupling-results.txt`, `build/sdram-manual-a11-results.txt`
and `build/sdram-auto-after-manual-results.txt`. No solder short is confirmed.

### Lower-speed tests without external instruments

The user has neither an oscilloscope nor a multimeter. Continued through
ST-Link using temporary clock changes while the SDRAM was in self-refresh;
the source configuration and normal clock are restored by reset.

At 32 MHz SDCLK, used CKPER from the existing 64 MHz HSI oscillator as the
FMC kernel, divided by two. The refresh count was recalculated to
`floor(32000000 * 0.064 / 8192) - 20 = 230`. The normal 32 MiB test again
failed on pass 5 at 0xC0000000, expected zero, actual 0x00080000.

Also tested 8 MHz SDCLK by using the existing PLL2 160 MHz VCO with R=10,
giving a 16 MHz FMC kernel divided by two. PLL2 was stopped before changing
its divider, the R output was enabled, and PLL2 ready was checked before
selecting it. Refresh count was 42. An immediate two-address debugger test
still showed 0xC0000000 and 0xC0200000 aliasing.

The 8 MHz full-memory run also failed its first constant-pattern pass at
address 0xC028013C (expected zero, actual 0xFFF5FFB0). This is an additional
failure at that diagnostic clock and must not be represented as the same
fifth-pass result seen at 32/100 MHz. Reset restored the original clocks after
these tests; no low-speed clock configuration was written into firmware.

These results weaken a simple high-speed-only timing explanation. They do not
identify a short, a specific damaged component, or a confirmed controller
defect. The software-controlled A11 full-capacity test remains a diagnostic
success, while ordinary automatic addressing is unresolved.
