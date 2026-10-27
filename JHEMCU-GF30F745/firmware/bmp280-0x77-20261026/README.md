# JHEMCU GF30F745: BMP280 at 0x77

AI-assisted local hardware-definition change, built from the existing WSL checkout.
Only the BMP280 hardware probe and driver enable were added. Existing custom IMU edits were retained.
No flight-controller parameter, controller, calibration, board-ID, bootloader, or storage-layout changes were made.

## Firmware to upload

Use `arducopter-bmp280-0x77.apj` through Mission Planner / the existing ArduPilot bootloader.
Do not upload a combined `with_bl.hex`, perform a full-chip erase, reset parameters, or change vehicle type.
Application starts at 0x08018000. Parameter storage remains on flash pages 1 and 2, below that address.
A normal application-only upload is expected to retain parameters, but verify against your saved .param backup.

## Upload and verify

1. Keep your complete .param backup. Remove propellers for the bench check.
2. Connect the FC by USB. Disconnect the telemetry session in Mission Planner.
3. SETUP -> Install Firmware -> Load custom firmware. Select this .apj.
4. Follow the prompt to unplug/replug the USB connection if requested. Keep power connected during programming.
5. Do not force a board-ID mismatch. This application is for custom board ID 1050.
6. Reconnect and let parameters load. Compare them with the saved .param file; do not reset to defaults.
7. Check BARO1_DEVID is nonzero and inspect press_abs / press_temp, or SCALED_PRESSURE in MAVLink Inspector.
   If a second barometer exists, also check the second instance. Instance selection is not assumed.
8. Keep the FC stationary during startup ground-pressure calibration. Then gently raise/lower the FC and inspect altitude.
9. Check your existing RC mapping/calibration, IMU calibration, TFmini and SERIAL/MAV stream settings before use.

BMP280 wiring: FC I2C1 PB6=SCL, PB7=SDA, shared ground. CSB and SDO tied to 3.3 V selects I2C address 0x77.
The module power input must match its board specification. Never connect the logic pins to 5 V.

The BMP280 is explicitly probed at startup; BARO_PROBE_EXT is not required for this fixed probe.
No BARO_PRIMARY or EKF source changes have been made. Sensor detection and altitude response require hardware testing.
If no sensor is found, inspect startup Messages and verify power, pin labels, pull-ups and address rather than changing EKF settings.

## Rollback

`previous-build/arducopter.apj` is the previous WSL build, saved before this change.
Upload it using the same custom-firmware method if needed. It is not a readback from the connected FC.
`previous-build/hwdef.dat` and `hwdef-change.patch` preserve the source change for review/reversal.

## Validation

`./waf configure --board JHEMCU-GF30F745` and `./waf copter -j4` passed with GCC 10.2.1.
Board ID, source identity, linker layout, parameter-storage settings, BMP280 ELF symbols and APJ payload were checked.
No hardware, flight or SITL testing was performed. See build-info.json for hashes and details.
