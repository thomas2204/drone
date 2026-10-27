# BMP280 + MAVLink GPS firmware

This AI-assisted build adds `AP_GPS_MAV_ENABLED=1` to the previous BMP280-enabled
JHEMCU-GF30F745 firmware. BMP280 probing remains on I2C bus 0, address 0x77.
Board ID 1050, application start, linker layout, parameter storage and the existing
custom IMU source changes are preserved. See build-info.json for hashes and checks.
Hardware reception and flight performance have not been verified.

## Load and configure

1. Save a fresh Mission Planner parameter backup. Keep the previous BMP280 APJ for rollback.
2. Flash the STM32 companion with `D:/projects/drone/build/Debug/projects.elf`.
3. In Mission Planner, disconnect telemetry and use Install Firmware / Load custom
   firmware to upload `arducopter-bmp280-mavlink-gps.apj` from this folder. Follow its
   USB connection prompts. Use the normal bootloader application upload. Do not
   force a board mismatch, erase the whole chip or use a combined bootloader HEX.
4. Reconnect and compare parameters/calibrations with the saved backup. Normal
   application flashing is expected to preserve them; this build does not relocate storage.
5. Set `GPS1_TYPE = 14` (MAVLink), write parameters and reboot the FC. This build
   uses the modern GPS1_TYPE name. The companion sends gps_id=0, targeting GPS1.
6. Keep `SERIAL3_PROTOCOL = 2`, `SERIAL3_BAUD = 460`, `SERIAL3_OPTIONS = 0` for
   the existing MAVLink connection. SERIAL3 is not being changed into a raw GPS port.
7. Power both boards and the BE-182. Outdoors, wait for a valid fix. Confirm pressure
   still works, and check Mission Planner's GPS fix, satellite count and map position.
   GPS_RAW_INT (message 24) in MAVLink Inspector shows the FC's accepted GPS1 data.

## Companion output

`GPS TX=<count> ERR=<count> LastFix=<type>` reports local UART results, not an FC
acknowledgment. With fresh synchronized 3D data, TX should increase, ERR should stay
zero and LastFix should be 3. Confirm reception separately in Mission Planner.
LastFix is the last successfully transmitted type, so it can remain 3 after the
GPS stops updating. Stale data is not repeatedly transmitted to hide a lost receiver.

The existing yaw/roll/armed output, GPS position and raw GSA/RMC diagnostics remain.
The FC image can be rolled back to the previous BMP280 APJ if necessary; restore
GPS1_TYPE to its previous value when rolling back and verify the saved parameters.

## Data handling and limits

- Each accepted RMC sample is transmitted once, at a maximum of 5 Hz. The observed
  receiver rate is roughly 1 Hz. This implementation does not create extra fixes by
  repeating old data. First establish reception; a faster real receiver update rate
  and full estimator checks remain before using this GPS for flight navigation.
- GGA and RMC UTC timestamps must match exactly. GSA must arrive within 500 ms of
  RMC because GSA carries no UTC timestamp. This is an arrival-time approximation,
  not a guarantee of GSA's measurement epoch. Data older than 1.5 seconds is withheld.
- New explicit no-fix reports send fix_type=1. Samples awaiting matching data are
  withheld; complete loss of GPS input is left to ArduPilot's GPS timeout handling.
- Latitude/longitude use degrees times 1e7. Altitude is metres above mean sea level,
  not barometric relative altitude or TFmini ground distance.
- Horizontal N/E velocity is calculated from RMC ground speed and course only when
  both exist. Missing course marks horizontal velocity ignored; it is not vehicle yaw.
- Vertical velocity and metric accuracy fields remain ignored. DOP is unitless and
  is not substituted for metre accuracy. Missing measurements are not reported as
  perfect zero measurements.
- UTC date/time is converted to GPS week and milliseconds-of-week, including the
  18-second GPS-minus-UTC offset in effect since 2017-01-01. Conversion supports
  2017..2099 and needs updating if a future leap second changes this offset.
- The source driver may retain earlier velocity values when later packets omit
  velocity. An accepted 3D fix alone does not establish suitability for navigation.

References:
- https://ardupilot.org/mavproxy/docs/modules/GPSInput.html
- https://mavlink.io/en/messages/common.html#GPS_INPUT

## Validation completed

- STM32 Debug build and GPS GGA/GSA/RMC parser tests.
- GPS_INPUT conversion tests: UTC/GPS time, field units, fix mapping, unavailable
  fields, explicit loss of fix, stale/mismatched samples, tick rollover, and a
  MAVLink v2 encode/parse/decode roundtrip with CRC checking.
- JHEMCU-GF30F745 Copter build, linked MAVLink GPS and BMP280 drivers, APJ payload
  comparison, unchanged linker/storage layout and unchanged custom IMU patches.

Flashing and end-to-end hardware verification remain user steps.
