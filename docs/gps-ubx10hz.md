# BE-182 GPS at 10 Hz through the STM32 companion

Prepared 2026-10-28. Target: this project's **STM32H743 companion**, not the JHEMCU flight controller.

## What changes after flashing the STM32

The GPS task sends a u-blox M10 CFG-VALSET command after startup. It requests a 100 ms measurement interval, one navigation solution per measurement, NAV-PVT and NAV-DOP output on the GPS's UART1, UBX output enabled, and NMEA output disabled. Configuration is RAM-only: the command does not save to flash or backup RAM, alter baud rate, or change constellations.

NAV-PVT supplies position, MSL altitude, North/East/Down velocity, fix validity and position/velocity accuracy. NAV-DOP supplies actual horizontal and vertical DOP. The bridge forwards a new valid solution only when PVT and DOP refer to the same GPS epoch. It sends MAVLink GPS_INPUT over STM32 USART1, at the receiver's rate; the old 5 Hz forwarding limit is removed. Course of travel is never used as vehicle yaw.

No-fix PVT is forwarded as no fix without waiting for DOP. Corrupt UBX packets are rejected. Repeated PVT epochs do not refresh freshness. After UBX reception starts, data aged 500 ms or more stops forwarding; old NMEA data cannot silently replace it. Before the first PVT, the existing synchronized NMEA path remains available.

Bluetooth still prints about once per second. The GPS receive/forwarding tasks run separately, so Bluetooth's print interval does not limit the GPS to 1 Hz.

## Wiring and firmware installation

Existing interfaces remain:

- BE-182 at **115200, 8N1** on STM32 UART5: GPS TX to STM32 RX and GPS RX to STM32 TX, with common ground. GPS RX must be connected for configuration commands to work.
- FC MAVLink on STM32 USART1 at **460800**.
- HC-05 on STM32 UART7 at **115200**.

1. Build/flash this STM32 project using the existing STM32Cube/ST-Link configuration, or load the packaged `stm32h743-gps-ubx10hz.elf` with ST-Link. The package's HEX is an alternative for STM32CubeProgrammer.
2. Power the GPS and STM32 together. If the GPS is powered much later, reboot the STM32 after powering the GPS.
3. Keep the vehicle disarmed for this data-path check. Open the HC-05 terminal and inspect the diagnostics below.

This package is **not an ArduPilot APJ**. Do not install it through Mission Planner onto the FC. The FC firmware, parameters, compass calibration and yaw-source settings were not changed by this work. GSF has not been enabled.

## Expected Bluetooth diagnostics

Example only; counters and measured rates vary:

```text
GPS TX=... ERR=0 LastFix=3
GPS PVT=... DOP=... RX=10.0Hz TX=10.0Hz Step=100ms Age=...ms Bad=0 Dup=0 Gap=0 CFG=ACK
GPS UBX Lat=... Lon=... AltMSL=... m Sat=... Fix=3
GPS VEL N=... E=... D=... mm/s hAcc=... vAcc=... mm sAcc=... mm/s
```

- `CFG=ACK`: receiver acknowledged the configuration. Actual measured rate and timestamp spacing must also be checked.
- `PVT` / `DOP`: accepted NAV-PVT / NAV-DOP packet counts. Both should increase by roughly 10 per second.
- `RX` / `TX`: new PVT epochs received / GPS_INPUT transmissions per second, measured over the print interval. The first rate line is zero while establishing a baseline. Small variation around 10 Hz is expected from sampling.
- `Step=100ms`: spacing of successive receiver solution timestamps. This verifies fresh measurements rather than repeated transmission of one fix.
- `Age`: time since the latest new PVT. It should stay below 500 ms; normally well below 100 ms at 10 Hz.
- `Bad`: checksum or decoded-payload errors. It should not keep rising.
- `Dup`: repeated PVT epochs; `Gap`: intervals other than 100 ms, including a possible startup transition. Watch whether these keep increasing after startup.
- `Fix=3`: receiver's 3D fix type. A no-fix line can be normal before outdoor acquisition. `LastFix` is the MAVLink fix type transmitted to the FC.
- `N/E/D`: velocity in millimetres per second, with Down positive. Small nonzero values at rest can be receiver noise. `hAcc/vAcc` are reported horizontal/vertical accuracy estimates in mm; `sAcc` is velocity accuracy in mm/s.

`TX` counts a successful local UART transmission, not FC acknowledgement or EKF use. After the Bluetooth check, verify reception/update timing in Mission Planner's MAVLink Inspector / GPS_RAW_INT or a new FC log before changing yaw-source settings.

## If the check fails

- `CFG=NAK`: the receiver rejected the configuration. Record the complete diagnostics; do not assume it is running at 10 Hz.
- `CFG=TIMEOUT`: no matching ACK was received after three attempts, two seconds apart. Check GPS RX wiring, power and 115200 baud; restart the STM32 after correction. The receiver could have applied a command whose ACK was lost, so inspect PVT/DOP rate too.
- `PVT` increasing but `DOP=0`, or `TX` staying zero with a valid fix: the bridge waits for matching fresh DOP. Record the counters and configuration status for diagnosis.
- `GPS UBX STALE`: new receiver epochs stopped. Do not treat the last position as live data.
- A valid fix is needed outdoors before evaluating the GPS navigation data; a successful build or ACK does not establish flight readiness.

If reverting to old NMEA-only STM32 firmware, restore the GPS's saved configuration by resetting/power-cycling the GPS as appropriate for its backup power. Flashing/restarting the STM32 alone does not restore GPS settings while the GPS remains powered. This implementation does not issue any persistent-save command.

## Validation performed

- Debug ARM build passed with GNU Arm 14.3.1; FLASH 120676 bytes, DTCMRAM 26904 bytes, DMA SRAM 3072 bytes.
- Native tests passed for NMEA compatibility; UBX signed fields, independent frame/config fixtures, checksum rejection, resynchronization, invalid fixes and ACK/NAK; conversion, synchronized DOP, freshness, signed NED velocity, UTC/GPS time, ignore flags and MAVLink serialization/parsing roundtrip.
- ARM compile-time checks confirmed GPS_INPUT structure size 65 bytes and yaw extension offset 63. MinGW tests use `-mno-ms-bitfields` to match that packed layout.
- Hardware configuration acceptance, measured 10 Hz reception, FC reception and GSF operation remain to be verified on the user's hardware. Neither board was flashed during preparation.

Run native tests from PowerShell with `./tests/run-gps-tests.ps1 -Compiler C:/MinGW/bin/gcc.exe`. Build firmware with the project's existing CMake Debug preset/build directory. Existing warnings from generated MAVLink headers remain.

## Protocol reference

[u-blox M10 SPG 5.10 Interface Description](https://content.u-blox.com/sites/default/files/u-blox-M10-SPG-5.10_InterfaceDescription_UBX-21035062.pdf): NAV-PVT, NAV-DOP, CFG-VALSET, CFG-RATE, CFG-MSGOUT and CFG-UART1OUTPROT. This command requires the M10 configuration interface; its ACK and observed output are the hardware compatibility check.
