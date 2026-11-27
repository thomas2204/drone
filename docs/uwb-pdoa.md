# Two-antenna PDoA bench setup

The drone now uses channel 5, the existing 4096-symbol preamble / PAC32 /
850 kbps PHY, STS mode 1 with 128-symbol Super Deterministic Codes (SDC),
PDoA mode 3 and automatic RF1-to-RF2 switching. The beacon uses the same
over-the-air settings, PDoA mode 0 and its existing single RF1 antenna.
Both boards must run these matching firmware versions.

SDC intentionally avoids STS key/IV synchronization for bench bring-up.
It does **not** provide cryptographically secure ranging or target authentication.
No flight commands or following controller are enabled by this change.

## Build and load both boards

From the repository root in PowerShell, with CMake, Ninja and the Arm GNU
toolchain available:

```powershell
cmake --preset Debug
cmake --build --preset Debug
./scripts/build-uwb-beacon.ps1
python tests/test_uwb_pdoa.py
```

Drone output: `build/Debug/projects.elf`.
Beacon output: `build/beacon-pdoa/Beacon.elf` and `Beacon.bin`.
The beacon build script uses the checked-in CubeIDE object manifest, avoiding
the generated makefiles' absolute paths to another developer's machine.
Flash each ELF to its corresponding board with your normal debugger.
This change does not automatically flash either board.

## Antennas and calibration

1. With power off, connect matching UWB antennas to RF1 and RF2 on the drone.
   Use matching polarization, rigid mounting and stable cables. The beacon
   keeps one antenna on RF1. A two-port splitter on one RF input is not equivalent.
2. Keep the antenna phase centers at most approximately 23.1 mm apart on channel 5
   for this simple unambiguous model. Connector spacing is not phase-center spacing.
   Larger baselines require an ambiguity resolver, which is not implemented here.
3. Start with the beacon several metres away, unobstructed, at the antenna pair's
   height. Place it directly ahead, perpendicular to the antenna baseline.
4. Bluetooth prints the existing distance and a separate line such as:

   ```text
   UWB phase: -12500 mdeg | bearing: send uwb zero facing target
   ```

   `mdeg` means millidegrees. This is **phase difference**, not target angle.
   Failed STS/status/TDoA checks print `UWB angle: no valid phase` even when
   an Ipatov distance is available. A failed exchange invalidates phase immediately.
5. The current build assumes the user's approximately 20 mm antenna spacing
   (`UWB_ANTENNA_SPACING_M = 0.020f` in `Core/Inc/uwb_angle.h`). Keep the beacon
   directly ahead, wait at least one second for the filter to fill, then send
   `uwb zero` through Bluetooth. This captures the current filtered phase as
   forward and enables output such as `UWB angle approx: +12.3 deg`.
   Zero fails if there is no fresh full filtered window. Wait for
   `UWB: forward zero saved to SD` before removing power. The board loads the
   latest saved offset from `UWBZERO.BIN` at startup with the same SD card.
   If saving fails, the new offset is usable until reboot. Repeat calibration
   after changing the antenna mounting or cables.
6. Check known angles on both sides and at multiple distances. Positive angle
   follows the chip's RF1/RF2 phase convention; verify the sign physically before
   interpreting it as right/left. If it is reversed, consistently swap RF1/RF2,
   then recalibrate. Do not change cables or mounting after calibration.

The ideal model is `asin(wrap(phase - offset) * wavelength / (2*pi*spacing))`.
Invalid spacing, unsupported channels and impossible sine values are rejected,
not clamped. The chip's hardware phase offset is zeroed so offset compensation
is applied exactly once in software. Real antennas may need a measured angle
lookup table; a single offset is an initial model, not a guaranteed accuracy.

One baseline gives one directional constraint and has front/back ambiguity.
Bearing assumes a front tracking sector and known plane; two antennas do not
provide arbitrary 3D azimuth and elevation. Drone roll/pitch and target height
must be accounted for in a future tracking estimator.

## Coarse angle filtering

The displayed angle uses a five-sample circular median at the actual UWB task
rate (roughly 7-10 samples/second), not the once-per-second Bluetooth log rate.
It suppresses up to two isolated spikes in a five-sample window and handles
phase wrapping at +/-180 degrees. A sustained change is accepted as the new
direction after it becomes the majority, rather than locking to an old value.
There is roughly a few tenths of a second of step response delay.

A gap over 500 ms between valid phase samples clears history. Five new valid
samples are required afterwards. Invalid current readings never produce a held
old angle. Filtering does not correct persistent multipath bias or prove which
physical side corresponds to a positive reading. Check a left/right movement
once before interpreting the sign. This is approximate front-sector angle
display only; no automatic flight control was added.

## Diagnostics and ranging

`UWB_RangeSnapshot` exposes raw PDoA, STS quality, both STS segment statuses,
full signed 41-bit TDoA, rejection count, mode register readbacks, phase validity,
calibration state and bearing validity. Raw phase validity is separate from
distance validity. Check the sample age as well as the validity flags.
`config_verified` now also checks automatic switching, STS mode/length and PDoA mode.

PDoA is accepted only for the expected response with a valid distance, good STS
quality, both valid STS statuses, CIA completion without CIA/STS errors, and
absolute TDoA no greater than the driver's `DWT_VALID_TDOA_LIMIT` (100).
Two antennas alone cannot guarantee a good direct-path angle under multipath.

Both peers explicitly read Ipatov timestamps for TWR: this SDK ignores the
segment argument in the generic timestamp APIs. The existing 8 ms beacon
response delay and drone receive/deadline windows accommodate the added STS.
The drone's primary port is now RF1 rather than the earlier forced RF2;
recalibrate antenna delays/distance bias for the assembled hardware.

Host tests exercise phase-to-angle geometry, invalid calibration, signed TDoA
and radio error/quality handling. Builds and host tests do not validate actual
RF switching, antenna geometry or accuracy. Verify these on the bench before
using measurements in a flight controller.

References: [Qorvo API guide](https://forum.qorvo.com/uploads/short-url/xD3TlXKvkujjdUaJWXv2b4E7GQN.pdf),
[Qorvo explanation of SDC without KEY/IV](https://forum.qorvo.com/t/dwm3000-collison-avoidance-on-low-power-anchor-system/22935).
