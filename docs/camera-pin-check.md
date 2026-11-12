# Camera pin investigation, 2026-11-12

The user requested camera/I2C/DCMI pin checks before capture, with the OV2640
plugged in initially. Tests used ST-Link and GPIO digital readback. No resistance
measurement or oscilloscope trace was available. A logic-level fault is not
automatically proof of a solder short.

## DCMI and control signals

Held the camera in reset (PG6 low) and power-down (PG7 high). Configured all
eleven DCMI signals as MCU inputs and checked both internal pull-up and pull-down
bias. All followed both levels:

| Signal | MCU pin | J1 pin |
| --- | --- | --- |
| D0 | PH9 | 4 |
| D1 | PH10 | 14 |
| D2 | PH11 | 5 |
| D3 | PH12 | 13 |
| D4 | PE4 | 6 |
| D5 | PD3 | 12 |
| D6 | PE5 | 7 |
| D7 | PE6 | 11 |
| HSYNC | PA4 | 16 |
| PIXCLK | PA6 | 8 |
| VSYNC | PG9 | 17 |

No stuck-to-rail indication was observed on these eleven signals under these
test conditions. With input pull-ups, individually pulled each eligible signal
low using open-drain GPIO. Repeated with nearby high-impedance nets also biased
(PD4, PE3, PG10, PG12, PH7, PH8). Each selected signal alone read low. No hard-short
coupling indication appeared among those seventeen signals. Unbiased neighbouring
inputs initially showed incidental changes; those disappeared when biased, so
they were not treated as shorts.

Reset PG6/J1.15 initially read low and read high when released against R3's
10k pull-up while power-down stayed high. Power-down PG7/J1.9 initially read
high and read low when released against R4's 10k pull-down while reset stayed
low. Both control lines reached both expected logic levels.

J1 pin 10 is unconnected in the PCB/schematic. No MCU can directly test an
oscillator/XCLK signal on that unconnected pad. PIXCLK at J1 pin 8 was tested
separately and passed the static checks.

## I2C: confirmed board-side stuck-low SCL

The camera's SCCB/I2C signals are SCL PB8/J1.2 and SDA PB9/J1.3. R1 and R2
are 4.7k pull-ups to +3.3V. The raw GPIO reads must be masked with 0x100 for
PB8 and 0x200 for PB9. An initial verbal summary mistakenly called both lines
high; that interpretation was corrected. For example 0xFE18 has PB8=0,
PB9=1, not both high.

With the module connected:

- PB8 remained low under both weak pull-up and pull-down bias.
- PB9 remained high under both weak biases, consistent with its external pull-up.
- Loaded the camera firmware with `camera_snapshot_save_to_sd=0` before its
  task ran, so no SD file was opened or written.
- The first register write (bank select register 0xFF at slave address 0x30)
  timed out: HAL_I2C_ERROR_TIMEOUT=0x20. No sensor ID or frame was captured.
- With I2C PE disabled, reset asserted and power-down high, both pins released
  as GPIO inputs: SCL stayed low, SDA was high. SDA could be pulled low and
  released high independently. SCL could not be observed high.

The user then powered off, removed the OV2640, and powered the bare board again.
Repeated the isolated GPIO test with no module attached:

| Condition | PB8 SCL | PB9 SDA | Raw GPIOB IDR |
| --- | --- | --- | --- |
| Both released as inputs/pull-ups | 0 | 1 | 0x0000FE18 |
| SCL open-drain low | 0 | 1 | 0x0000FE18 |
| Both released again | 0 | 1 | 0x0000FE18 |
| SDA open-drain low | 0 | 0 | 0x0000FC18 |
| Both released at end | 0 | 1 | 0x0000FE18 |

This establishes a board-side stuck-low SCL, independent of the camera module
and I2C peripheral driving the line. A ground short, wrong connection/component
or MCU-pad fault remains possible. The digital tests do not prove a particular
solder bridge or provide a resistance value. Do not call a solder short confirmed.

The design's SCL path is U1 PB8/pad 167 -> CAM_SCL -> J1 pad 2, with R1 to
+3.3V. J1 pad 1 is GND and is an important nearby inspection point. Check the
header's solder side, R1 and the MCU PB8 area with power removed. The presence
of this fault does not establish the cause of the separate SDRAM A11 fault.

Evidence is retained in:

- `build/camera-passive-pins-results.txt`
- `build/camera-neighbour-passive-results.txt`
- `build/camera-neighbour-coupling-results.txt`
- `build/camera-functional-pincheck-results.txt`
- `build/camera-i2c-coupling-results.txt`
- `build/camera-i2c-bare-board-results.txt`

The camera firmware is now installed. The diagnostic request used SD saving
disabled; that RAM setting resets to its normal default of enabled on reboot.
With the camera absent or I2C failing, the task stops before SD access.
At handoff, request and SD-save flags were both zero, reset was asserted,
power-down high, I2C PE disabled and PB8/PB9 left as input pull-ups for quiet
inspection. These temporary diagnostic pin settings revert on reset.

## After the user's repair

The user confirmed finding and repairing a short, then reconnected the module.
Retested the isolated SCL/SDA GPIOs with the camera in reset/power-down:

| Condition | PB8 SCL | PB9 SDA | Raw GPIOB IDR |
| --- | --- | --- | --- |
| Both released | 1 | 1 | 0x0000FF18 |
| SCL open-drain low | 0 | 1 | 0x0000FE18 |
| Both released again | 1 | 1 | 0x0000FF18 |
| SDA open-drain low | 1 | 0 | 0x0000FD18 |
| Both released at end | 1 | 1 | 0x0000FF18 |

The previous stuck-low SCL is resolved. The two lines now switch independently.
The running image did not initially match the camera symbols, so the dedicated
camera ELF was reloaded before evaluating the application report.

With SD saving disabled, the camera consistently returned PID 0x2642. However,
configuration stopped at DSP register 0x2E, following the initial 0x2C=0xFF
write in the vendor table. The failure was ARLO at 400 kHz and timeout at
100 kHz. Separate STOP-delimited SCCB reads and combined register/value writes
with 1 ms write settling did not resolve it. A temporary approximately 12.5 kHz
bus timing (0xF0F4F9F9 at 100 MHz kernel) also timed out at 0x2E. Before the
0x2E transaction, both lines were high and the peripheral reported ready.

No DCMI capture was reached and no SD card was accessed in these tests. The
camera module's exact clock arrangement and pinout remain to be established;
the PCB does not route a camera XCLK. The next hardware information needed is
a clear front/back view of the module to identify its oscillator and connector.
Do not infer another solder short from this configuration failure alone.

Evidence: `build/camera-i2c-after-repair.txt`,
`build/camera-first-capture-after-repair.txt`,
`build/camera-100khz-after-repair.txt`, `build/camera-before-reg2e.txt`,
`build/camera-sccb-after-repair.txt`, `build/camera-slow-sccb-after-repair.txt`.

### Online module references and SCCB diagnostic

The user requested using online module images instead of supplying a photo.
Found the common 18-pin, two-row layout and checked it against the actual J1
footprint pad coordinates. The custom footprint numbers one row 1..9 and the
opposite row 18..10; this differs from ordinary interleaved header numbering.
For example PCB pad 2 is the second position on the GND/SCL/SDA row, equivalent
to pin 3 in a conventional interleaved diagram. PCB pad 18 is the 3.3V position,
equivalent to conventional pin 2. The physical signal positions agree with
the common reference, including NC opposite PWDN at the final row position.

[ALIENTEK's module documentation](https://github.com/alientek-module/CAMERA-module_ATK-MC2640)
documents an 18-pin OV2640 module with an integrated active oscillator and a
compatible control/power header layout.
[EmbedFire's documentation](https://doc.embedfire.com/mcu/stm32/f407batianhu/std/zh/latest/book/DCMI_OV2640.html)
also shows an OV2640 module with its own 24 MHz oscillator, but a different
20-pin connector. Thus an unwired MCU XCLK does not establish a missing camera
clock. The exact oscillator on this user's module has not been measured or
identified; these references are not proof of the exact module variant.

Tested open-drain software SCCB (diagnostic flag enabled, SD disabled). It read
0x2642 but also timed out at the vendor initialization prefix's register 0x2E.
After the failure, both GPIO outputs were released, but SDA read low. This
does not prove a new solder short; earlier static independent-line tests passed.

As a controlled debugger-only experiment, skipped the table's first three
entries (DSP bank select, reserved 0x2C=0xFF and 0x2E=0xDF) by advancing the
table pointer. The remaining configuration transactions completed without
I2C errors, but DCMI timed out: zero frames, zero VSYNC callbacks and zero
captured bytes. The skip is not a permanent source change and has not produced
a valid image. Do not describe it as a validated initialization workaround.

Evidence: `build/camera-software-sccb-result.txt` and
`build/camera-prefix-skip-result.txt`. No SD writes were requested in either run.

### Wiring correction: two cameras share I2C1

The user subsequently confirmed that both cameras are physically wired to
PB8/PB9 (I2C1). The schematic/netlist labels a thermal-camera connection on
PH4/PH5 (I2C2), but that does not describe the user's actual shared-bus wiring.
The thermal task's existing `hi2c1` placeholder is therefore consistent with
the stated actual wiring; it must not be changed to I2C2 based only on labels.

Earlier OV2640 tests did not establish that the second physical bus device was
absent or inactive. Disabling its firmware task does not disconnect it from
the bus. Likewise, removing only the OV2640 is not full bare-bus isolation if
the second camera remains attached. The user independently confirmed finding
and repairing a short, and repaired SCL/SDA levels were verified; that repair
is distinct from diagnosing the remaining initialization failure.

The second camera was identified as MLX90640. An earlier "ready" reply was
mistakenly treated as confirmation that it had been physically disconnected.
The user clarified that it was still connected. Therefore the earlier files
named `camera-ov-only-hardware-result.txt` and `camera-ov-only-software-result.txt`
must NOT be treated as isolated-bus evidence.

The user subsequently explicitly said "i now disconnected" in reference to
the MLX90640. Reran with a fixed copy of the camera ELF and SD saving disabled.
Both hardware I2C and software SCCB still read 0x2642 and failed at register
0x2E. These newly confirmed isolated results establish that MLX90640 does not
have to be connected for this failure to occur. They do not validate the
combined bus under load or identify the MLX90640's programmed address.

[Melexis's datasheet](https://www.melexis.com/-/media/files/documents/datasheets/mlx90640-datasheet-melexis.pdf)
specifies MLX90640 default address 0x33, distinct from the OV2640 address 0x30
used here. No thermal-camera address/EEPROM programming was performed.
Confirmed isolated evidence: `build/camera-confirmed-ov-only-hardware.txt`
and `build/camera-confirmed-ov-only-software.txt`.

An additional diagnostic samples PA6 (PCLK), PA4 (HREF) and PG9 (VSYNC) for
50 ms before configuration and after configuration. Exposed counts are
`pclk_edges`, `href_edges`, `vsync_edges` in the report. With the reserved DSP
prefix omitted in an opt-in diagnostic, configuration transactions completed
but no edges were observed in either window and capture timed out. This is
not proof that the oscillator is absent: the sequence is experimental and
the sensor can gate its outputs. The source sequence is not silently changed;
`camera_snapshot_skip_dsp_prefix` defaults to zero and flags diagnostic runs.
Evidence: `build/camera-confirmed-ov-only-clock-result.txt`.
