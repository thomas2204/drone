# GPS reception diagnostics

## Current normal Debug build: 5 Hz B1C comparison

The normal `Debug` configure preset now explicitly selects
`GPS_DIAGNOSTICS=ON`, `GPS_BDS_TEST_PROFILE=2`, and
`GPS_MEASUREMENT_PERIOD_MS=200`. Build and flash the project normally.
The older packaged images described below are the earlier 10 Hz comparisons;
they are not updated for this test.

Compared with the preceding 10 Hz B1C Debug build, only the requested
measurement interval changes. Signal settings, power settings, navigation
filters, UART rates and output-per-solution settings remain unchanged.
Expected output after configuration: `CFG=ACK BDS-test=2 targetStep=200ms`,
actual `RX` about 5 Hz, `step=200ms`, `BDS-B1I=0 BDS-B1C=1`.
The gap counter now compares receiver timestamps against the selected interval.
The 500 ms freshness limit remains compatible with 200 ms measurements.

Use the same stationary, disarmed outdoor setup for 3–5 minutes and compare
BDS/GAL listed, locked, ephemeris and used counts, plus accuracy and drift.
Changing rate is a diagnostic comparison, not a confirmed remedy or flight check.
To return to the 10 Hz B1C experiment, change the Debug preset's
`GPS_MEASUREMENT_PERIOD_MS` to `100`, reconfigure, build and flash.

Native tests pass for all three profiles at both 100 ms and 200 ms, checking
that only the interval/checksum bytes differ. The 5 Hz ARM Debug build passes.
This 5 Hz firmware has not been flashed or verified on hardware by the agent.

## Earlier diagnostic builds

Built 2026-11-12 for the STM32H743 companion, with `GPS_DIAGNOSTICS=ON`.
Baseline firmware: `build/Debug/projects.elf`; packaged ELF/HEX/BIN in
`build/gps-diagnostics/`. BIN load address is `0x08000000`.

The additional receiver commands are read-only RAM CFG-VALGET, MON-VER,
NAV-SAT and MON-RF polls. The existing RAM-only 10 Hz startup configuration
remains in place. No additional constellation/filter changes or persistent
saves are sent by the baseline (`GPS_BDS_TEST_PROFILE=0`). SD initialization failure no longer stops GPS startup;
`sd_boot_error` retains its boot error for debugger inspection.

## Bluetooth output

A diagnostic block is printed about every ten seconds. Wait at least 35 seconds
after boot for all configuration reads. Unknown values print `?`; they must not
be interpreted as disabled/zero. NAV-SAT and MON-RF are polled once per second.

- `GPS SETUP`: ACK/NAK/TIMEOUT of the existing 10 Hz startup command, attempts,
  acknowledgements and UART transmit errors. ACK alone does not prove 10 Hz.
- `GPS PVT`: measured new position epochs per second, receiver epoch spacing,
  age, decode errors, duplicates and gaps. The first interval includes startup.
- `GPS ACC`: fix validity, type, raw NAV-PVT flags, estimated horizontal/vertical
  accuracy in mm and speed accuracy in mm/s. HDOP/VDOP are scaled by 100;
  `sameEpoch=1` means DOP and PVT timestamps match. Values before the first PVT
  or DOP are not measurements. Use count, age and sameEpoch to assess freshness.
- `GPS NAV RAM`: dynamic model (0 portable, 2 stationary, 6/7/8 airborne),
  fix mode (1 2D-only, 2 3D-only, 3 auto), minimum/maximum satellites.
- `GPS FILTER RAM`: minimum CNO, signed minimum elevation, and number of
  satellites required above the separate CNO threshold before attempting a fix.
- `GPS OUTPUT RAM`: PDOP/TDOP thresholds scaled by 10, position accuracy
  threshold in metres, and whether the initial solution must be 3D.
- `GPS POWER RAM`: operating mode (0 full/continuous, 1 power-save on/off,
  2 power-save cyclic), ephemeris wakeup flag, peak-current limiting, acquisition
  limits, tracking on-time, position-update period and acquisition retry period.
  Power-save timing values must be interpreted with operating mode; an updateEph
  value of zero does not disable ephemeris decoding during full/continuous operation.
- `GPS EXTRA RAM`: acquisition retry period, use of BeiDou GEO satellites
  (PRNs 1–5 and 59–63), and signal attenuation compensation (0 off, 255 auto).
  The GEO flag cannot explain exclusion of every BeiDou satellite because it
  affects only those PRNs. Unsupported/unanswered keys remain `?`.
- `GPS HEALTH`: counts over all listed GPS/Galileo/BeiDou/QZSS satellites,
  including locked signals, available ephemeris and explicitly unhealthy flags.
- `GPS SV`: eight strongest entries across all constellations. `q=3` is detected
  but unusable, 4 code/time lock, 5–7 code/carrier/time lock. `health=0` unknown,
  1 healthy, 2 unhealthy. `orbit=0` none, 1 ephemeris, 2 almanac, 3 AssistNow
  Offline, 4 AssistNow Autonomous, 5–7 other; `eph`/`alm` indicate availability.
  These flags help diagnose exclusion but do not encode a unique exclusion reason.

`jam=0`, `ant=1`, `power=2` mean unavailable/unknown status, not a clean RF or
confirmed antenna-power result. A listed satellite is not necessarily acquired,
locked, healthy or used. Enabling its constellation does not guarantee use.

Protocol: [u-blox M10 SPG 5.10, UBX-21035062 R03](https://content.u-blox.com/sites/default/files/u-blox-M10-SPG-5.10_InterfaceDescription_UBX-21035062.pdf),
NAV-SAT, NAV-PVT and CFG-NAVSPG table 20.

## Controlled BeiDou comparison

The baseline preserves all signal settings and adds readouts. Check its power
and GEO readback first. Prepared comparison images are separate from baseline:

| Image | Requested signal settings | Rate |
| --- | --- | --- |
| `stm32h743-gps-diagnostics` | Preserve existing settings | 10 Hz |
| `stm32h743-gps-b1i-test` | BDS on, B1I on, B1C off, GLONASS off | 10 Hz |
| `stm32h743-gps-b1c-test` | BDS on, B1I off, B1C on, GLONASS off | 10 Hz |

The comparison profiles send a RAM-only CFG-VALSET containing the same rate and
UART output settings as baseline plus the four listed signal keys. GPS, Galileo,
QZSS, SBAS, power settings and filters are not modified. The captured baseline
already had BDS/B1I enabled, B1C disabled and GLONASS disabled. Do not enable
B1I together with B1C or GLONASS: the M10 integration manual forbids that mix.
The profiles deliberately keep rate at 10 Hz so a rate change cannot confound
the signal comparison. The exact module identity remains unreported (`MOD=`
empty), so compatibility must be established by ACK **and actual readback**.

`GPS SETUP BDS-test=0/1/2` identifies the requested build profile, not proof it
was applied. Check `CFG=ACK`, then `GPS CFG RAM` and `GPS SIGNAL` for the actual
values. NAK, timeout or mismatching readback makes the comparison inconclusive.
Signal changes restart the GNSS subsystem; the firmware waits two seconds
between attempts and begins diagnostic polling after eight seconds, exceeding
the documented 0.5-second post-ACK pause.

Use the same stationary, disarmed mounting for each test. Allow 3–5 minutes
and capture complete blocks, particularly BDS locked/eph/used, GAL listed,
CNO and hAcc/vAcc. Do not alternate profiles automatically during acquisition.
To return to the recorded B1I selection, use the B1I image and verify readback.
Flashing the preserve-settings baseline alone does not undo a previous RAM
signal change while the GPS remains powered. No BBR/flash save is sent.

Signal-combination reference:
[MAX-M10S integration manual, GNSS signal configuration](https://content.u-blox.com/sites/default/files/MAX-M10S_IntegrationManual_UBX-20053088.pdf).

## Verification

Native NMEA, UBX, diagnostic and GPS_INPUT tests pass. New fixtures cover
one-byte, signed elevation, two-byte and logical filter values, invalid response
widths/values, satellite health/orbit flags, four-byte power-save periods,
unknown-value formatting, and byte-for-byte baseline/B1I/B1C startup commands
(including mutually exclusive signal bits, checksums, capacity checks and RAM layer).
ARM Debug build passes; generated MAVLink packed-member warnings remain.
Hardware validation of this extension is pending: a fresh ST-Link connection
attempt returned `open failed` during preparation.
