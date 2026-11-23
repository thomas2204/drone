# GPS origin/startup code audit — 2026-11-19

Scope: current source, Debug build definitions, startup commit c6a8ae0,
and the previously examined November 18 FC logs. No FC parameter or flashed
firmware was changed during this audit.

## Findings

No defect explaining the logged speed-accuracy failures was identified in the
examined decoding/conversion/startup paths. This is not proof of correct live
transport or of the identity of the currently flashed binary.

- `gps_ubx.c` reads NAV-PVT sAcc at byte offset 68 as unsigned mm/s.
- `gps_input.c` converts it to GPS_INPUT speed_accuracy in m/s with factor
  0.001. It does not estimate uncertainty from satellite count, clamp it,
  average it, or substitute groundspeed. N/E/D velocities retain their signs.
- The bridge uses hMSL for altitude, metric hAcc/vAcc, and actual NAV-DOP.
  PVT and DOP must have identical iTOW. Measurements age out after 500 ms.
- `mk_app.c` forwards each received PVT epoch once after its matching DOP is
  available, retrying if local transmission fails. It sends no origin/home
  commands. No origin/home-setting command was found under Core.
- ArduPilot's MAVLink GPS backend copies speed_accuracy directly into its GPS
  state. It does not calculate this field from the FC's IMU.

## The earlier faster-startup change

`docs/gps-mg902-setup.md` records the replacement MG-902 receiver as M9 SPG
4.04 at 230400 baud. This differs from the earlier BE-182/M10 screenshots.

Current CMakePresets.json, Debug CMakeCache.txt and compile_commands.json agree:
profile 0, 200 ms measurements, diagnostics on. Profile 0 sends only a RAM-layer
rate/output configuration: CFG-RATE-MEAS, CFG-RATE-NAV, NAV-PVT/NAV-DOP output,
and UBX-on/NMEA-off. It sends no constellation change, receiver reset, assistance
clear, or persistent save. It can change the measurement rate/output, so it is
not a completely passive receiver connection.

The optimization's conditional constellation read/write logic is compiled only
for profile 3. It avoids unnecessary/repeated signal changes. Profile 3 is not
selected by this Debug build. AssistNow Autonomous was read for diagnostics;
the optimization did not enable or disable it. Its reported fast fix was from
a companion restart with the receiver still powered, not a verified cold start.

Diagnostic polling begins after 8 s; navigation reception/forwarding is not held
until then. Startup retries are finite. No mechanism in these paths explains
continuous origin blocking for minutes once the EKF GPS checks actually pass.
Neither supplied FC log is a controlled no-manual-origin startup.

## Verification

Added tests/test_gps_bridge.c. Independent UBX fixtures pass through the actual
checksum parser, GPS_BuildInput and MAVLink GPS_INPUT encoding/decoding. Checks
cover 0, 610, 1000, 1010, 1070 and 1560 mm/s uncertainty; signed NED velocity;
position/altitude/DOP; iTOW; stale/mismatched epochs; loss of fix; corrupt checksum;
and the exact whitelist of normal startup keys. The existing startup test also
passes, including suppression of repeated profile-3 signal changes.

Run from the repository root with MinGW GCC (the packing option matches ARM):

```powershell
gcc -std=c11 -mno-ms-bitfields -Wno-address-of-packed-member -ICore/Inc -IMAVLink tests/test_gps_bridge.c Core/Src/gps_input.c Core/Src/gps_ubx.c -lm -o build/test_gps_bridge.exe
.\build\test_gps_bridge.exe
gcc -std=c11 -ICore/Inc tests/test_gps_startup.c Core/Src/gps_ubx.c Core/Src/gps_diag.c -o build/test_gps_startup.exe
.\build\test_gps_startup.exe
```

Native tests do not exercise UART DMA, RTOS scheduling, power or RF conditions.
The remaining discriminating measurement is simultaneous receiver sAcc and FC
GPA.SAcc for matching epochs during a startup with no manual origin command.

References:
- [M9 SPG 4.04 interface, NAV-PVT and configuration](https://content.u-blox.com/sites/default/files/u-blox-M9-SPG-4.04_InterfaceDescription_UBX-21022436.pdf)
- [Installed FC revision's MAVLink GPS backend](https://github.com/ArduPilot/ardupilot/blob/9f648cca/libraries/AP_GPS/AP_GPS_MAV.cpp)
