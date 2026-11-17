# Replacement MG-902 setup (2026-11-12)

Live ST-Link inspection identified ROM CORE 4.04, SPG 4.04,
protocol 32.01 (u-blox M9). The receiver communicates at **230400 baud**,
not the previous module's 115200. A temporary companion-side scan found
valid UBX at 230400; 9600, 19200, 38400, 57600 and 115200 did not decode.

UART5 now starts at 230400, 8N1 in usart.c and projects.ioc.
The Debug preset now selects profile 0 (preserve all constellation settings)
and 200 ms measurements (5 Hz). This avoids sending the previous M10-only
BeiDou B1C experiment to the M9. Startup requests NAV-PVT and NAV-DOP,
UBX output on and NMEA output off;
configuration remains RAM-only. Rate/output configuration is reapplied at
startup; the normal Debug build sends no signal keys.
Receiver baud was not changed.

Built and flashed build/Debug/projects.elf through ST-Link. Live receiver
ACK, 200 ms PVT spacing, and zero UBX/UART errors were verified. Satellite
acquisition was checked indoors, antenna up, so an outdoor fix and flight
controller acceptance remain unverified.

Startup also now enables D2 SRAM2/SRAM3 clocks alongside SRAM1, covering
the DMA/heap placement across the declared 288 KiB RAM_D2 region.
This did not resolve reception at the wrong baud; the confirmed communication
fix was matching 230400 baud.

On the following outdoor inspection, clean 5 Hz reception continued but no
fix was present: three GPS satellites had lock and ephemeris, while two BeiDou
signals were detected but unusable. Readback showed GLONASS master enabled
but its L1 signal disabled. Profile 3 remedies that mismatch without changing
GPS/Galileo/BeiDou/QZSS, filters, power settings or serial baud. It is specific
to the identified M9 and must not be used for the old M10 B1I experiment.
ARM build and a native command fixture passed (RAM layer, exact two signal
keys, unchanged baseline fields, checksum, invalid profile and capacity).

Profile 3 was flashed and acknowledged. Live readback changed enabled mask
from 0x0b7f to 0x2b7f (GLONASS L1 bit enabled); known mask 0x2bff.
At approximately 121 s after restart, PVT remained no-fix, zero satellites
used, 200 ms epochs and zero decode errors. NAV-SAT listed two GPS entries,
one with lock at 29 dB-Hz and another without current tracking. Enabling
GLONASS did not restore acquisition during this observation. RF diagnostics
reported unavailable/unknown jam/antenna status, so they do not rule out
interference or antenna/power faults. Supply voltage and physical mounting
need independent inspection before assigning a hardware root cause.

## Startup optimization verified

Profile 3 now polls GLONASS master/L1 before startup configuration, with a
2.5 s bounded readback window. Already-correct or unknown settings use the
baseline rate/output command with no CFG-SIGNAL keys. A confirmed mismatch
allows one signal write; ACK retries omit signals to avoid repeated resets.
Legacy M10 comparison profiles retain their explicit experimental behavior.

The diagnostic readout now includes CFG-ANA-USE_ANA (key 0x10230001), shown
as ANA in GPS EXTRA. Hardware readback was known and 1: AssistNow Autonomous
was already enabled, so no ANA change or persistent save was issued.
This establishes configuration acceptance, not orbit-prediction readiness
or retention over a long power outage.

ARM build and tests/test_gps_startup.c passed. Native tests cover correct,
unknown, partial and mismatching signal readback, suppression of repeated
signal writes, exact RAM command payload/checksum, and ANA reply decoding.
After flashing, live debugger verification found signal_write_sent=false,
startup ACK, and a valid 12-SV fix at companion uptime 8 s with hAcc 2.819 m.
At 43 s: 10 SV, hAcc 2.829 m, zero decode errors, forwarded MAVLink fix type 3,
zero local forwarding errors. This was a companion-only restart while the
GPS stayed powered, not a cold-start timing test. A subsequent acquisition
under broader sky had already reached 6 SV before this optimization.

Reference: u-blox M9 SPG 4.04 interface description, CFG-ANA and CFG-SIGNAL.
No cold-start duration improvement is claimed until measured after a long
power-off interval.

## Orientation correction and conservative startup

The user subsequently found the antenna had been facing down and corrected
it. Earlier reception/receiver comparisons were therefore not controlled for
antenna orientation and cannot establish a receiver or interference fault.
On the next live inspection at uptime 123 s, signal_write_sent was true:
profile 3 had sent a signal change on this particular boot. This is distinct
from the earlier companion-only restart where it correctly skipped the write.
The recorded data do not establish why that boot's initial readback differed
or the time of first valid fix. A speed improvement was not demonstrated.

At inspection: valid 9-SV fix, hAcc 3.123 m, HDOP 1.43, strongest C/N0 37,
used GPS=6, BeiDou=1, QZSS=1, GLONASS=1, zero decode/forwarding errors.
Normal Debug now uses profile 0 so no constellation reset can be requested
by its startup VALSET. Profile 3 remains available only as an explicit setup
option. Profile 0 preserves receiver defaults/restored settings, including
GLONASS being off if the receiver restores that configuration after power
loss; it does not make GLONASS settings persistent. No aiding data is cleared
and no persistent receiver save is sent.

The profile-0 Debug firmware was built, native tests passed, and it was
flashed. At companion uptime 8.219 s: valid 13-SV fix, hAcc 2.216 m,
200 ms epochs, startup ACK, zero decode and local forwarding errors.
GPS remained powered during flashing, so this verifies preservation of
ongoing acquisition, not a cold-start speed comparison. No satellite
configuration or aiding-data save/clear was issued by the revised startup.

If reinstalling the old GPS, restore its known 115200 UART5 baud and select
a signal profile appropriate for that receiver. Firmware programming does
not automatically change the connected receiver's serial baud.
