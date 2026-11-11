# Distance to the beacon

The drone now runs an SS-TWR initiator in its own `uwb_range` task. The protocol
matches `UWB-beacon-CMake-version/Core/Src/beacon_twr.c`; that folder is reference
code and is not linked into the drone firmware. The Qorvo driver was copied into
`Drivers/UWB`, with its supplied license. No beacon source was changed.

## Wiring and radio

The implementation uses the existing SPI5 wiring: PF7 SCK, PF8 MISO, PF9 MOSI,
PF10 CS. PF6 IRQ is unused; the task polls status. SPI5 must be dedicated to UWB.
SPI divisors are selected from the actual kernel clock to stay under 2 MHz at
startup and 16 MHz after configuration. With the 100 MHz SPI5 kernel this gives
1.5625 MHz at startup and 12.5 MHz after configuration. The schematic connects
DWM1 pad 3 (RSTn) to PC1, which CubeMX initializes low. Startup configures PC1
as open drain with no pull-up, holds reset low for 2 ms, then releases it for
5 ms before probing. Writing SET to an open-drain output releases the line;
the MCU must never drive RSTn high in push-pull mode. After probing, the driver
also performs its software reset.

Both boards use channel 5, preamble 128, PAC8, preamble codes 9, SFD type 1,
6.8 Mbps, standard PHR, STS off and PDoA mode 0. The poll is 12 bytes including
hardware CRC; the response is 20 bytes, with little-endian poll-RX and response-TX
timestamps at offsets 10 and 14. The response must echo the poll sequence number.

The task starts an exchange about every 100 ms, allows roughly 6 ms for reception,
and has a 20 ms software deadline. It checks frame length, address/header, function
and sequence before computing distance. Clock offset correction and unsigned
timestamp subtraction handle crystal drift and low-32-bit timestamp rollover.
Invalid frames and failed exchanges are counted, with stale results marked invalid.
The distance limits are 0 to 1000 m as a sanity check, not a hardware range claim.

## Build and observe

Run `cmake --preset Debug` and `cmake --build --preset Debug` from the project root.
The firmware is `build/Debug/projects.elf`. Flash using your existing debug setup,
power the beacon, and view the existing UART7 Bluetooth output. Example:

```text
UWB distance=2.345 m RX=42 age=32ms
```

The radio uses the `rf_port` selection in `Core/Src/uwb_range.c` (currently
manual RF2, preserving the user's port test). The Bluetooth `RF` field is taken
from the same selection, rather than a fixed label.
`TX` counts confirmed radio transmit-complete events; `frames` counts received
CRC-valid frames before checking protocol/distance. `lastRSSI` is the received
channel power of the most recent received frame, in whole dBm, or unavailable
if no diagnostic measurement exists. It remains the last reading during dropouts.
These measurements help compare the two unlabeled antenna connectors: power
off before moving the antenna and compare at the same position/orientation.
Getting replies at approximately one metre does not alone identify the active
connector, because the inactive RF path can still couple some signal.

`UWB CFG` contains real register readbacks, refreshed once per second by the
UWB task. `ok=1` checks transmit power, channel, TX/RX preamble codes, SFD type
and the manual RF-port selector. It does not verify radiated power or antennas.
`UWB RSSIraw` prints channel-impulse-response power C, accumulated preamble
symbols N and receiver gain decision D for an independent RSSI calculation:
`10*log10(C*2^21/N^2) + 6*D - 121.7` for the current PRF64 Ipatov settings.

The driver's `ull_calculate_rssi` had a bit-extraction precedence bug. It now
uses the existing `ull_getrxcode` helper. For CHAN_CTRL=0x094A the old code
extracted 10 rather than 9; both select PRF64, so that bug did not change RSSI
for the current profile. The fixed-point RSSI calculation passed 116 test
vectors against an independent double-precision log10 reference.

Live-board readback on November 10, after flashing and verifying the diagnostic
build, showed ID DECA0312, TX_POWER FDFDFDFD, CHAN_CTRL 0000094A and
RF_SWITCH_CTRL 1C002100 (manual RF2), with configuration verification passing.
An initial sample had 61 transmissions, 61 valid ranges, no timeouts/errors,
distance 474 mm, C=12, N=108, D=0, RSSI approximately -88.4 dBm. This is a
close-range test; it does not establish acceptable operating range.

With the user confirming a fixed 1.5 m separation, the RF2 trial had 188
confirmed transmissions and no accepted replies (188 timeouts). The subsequent
RF1 trial had 181 confirmed transmissions and no replies (181 timeouts).
RF2 was restored and verified as RF_SWITCH_CTRL=1C002100. No transmit-power or
PHY changes were made. The RSSI shown during these no-frame intervals is stale,
not a new signal measurement. Before this controlled trial, a different
sample captured status 01810FF3, including RXFSL (frame-sync loss), with
CRC-valid replies on other attempts and no software range rejections.

These tests do not locate the failing direction. The F103 poll/reply/error
counters at close range and at 1.5 m are needed to distinguish missed outbound
polls from missing/corrupted inbound replies. A transmit-complete event confirms
the radio sequencer ran; it does not measure power radiated by the antenna.

`UWB NO RANGE` reports polls, successful ranges (RX), timeouts (TO), radio errors
(ERR), and rejected responses (BAD). `UWB init=-1` means probe failed; `-2` means
IDLE_RC timed out; `-3` means driver initialization failed; `-4` means radio
configuration failed. `init=1` means startup is still in progress. A failed init
leaves other tasks running; restart the board after correcting the fault.
`init=-5` means the optional UWB task could not allocate its memory. The UWB task
starts after two seconds at low priority, allowing GPS and Bluetooth to start first.
`init=-6` means the microsecond-delay cycle counter did not start.
Positive init stages identify the current operation: 2 probe, 3 reset/idle,
4 driver initialization, 5 radio configuration. The boot message
`BOOT: peripherals ready, starting RTOS` is sent on UART7 before RTOS setup.

`UWB_RangeGetSnapshot()` provides the same result to other application code.
Consumers must check `init_result == 0`, `valid`, and the age of `last_update_ms`.
The antenna delays (16385 initially) and TX RF settings are starting values;
calibrate delays on both boards before relying on absolute distance accuracy.
One fitted antenna is used for distance only; angle/PDoA is disabled.

## Verification

Run `./tests/run-uwb-tests.ps1` with a host GCC on PATH. Tests exercise distance,
timestamp rollover on both clocks, positive and negative clock offset, wrong
sequence/header/length, negative distance and out-of-range data. The firmware
build verifies driver integration. On-air operation still requires a hardware
test with both boards; the beacon's own receive-error bug can interrupt replies.

## Startup freeze diagnosed

The debugger backtrace stopped in `Error_Handler` from `MX_FREERTOS_Init`,
before any tasks started. The LED driver required a 32 MHz APB2 clock, but
`SystemClock_Config` selects HSI 64 MHz / 4 * 50 / 2 = 400 MHz SYSCLK,
200 MHz HCLK and 100 MHz APB2. TIM1 therefore runs at 200 MHz. LED startup
now derives ARR from the actual timer clock: 249 at 200 MHz, retaining
the required 800 kHz bit rate. It also supports the earlier 64 MHz TIM1
clock (ARR 79). `tests/run-led-tests.ps1` tests both clock configurations
and TIMPRE behavior against the actual LED driver. UWB remains enabled.

## UWB reset wiring verified

Source: `D:/pcbs/dronetestv1/test.kicad_sch`, DWM1 pad 3 on net
`/open drain gpio`, connected to U1 pad 33 (PC1). SPI and CS connections match
the firmware. The DW3000 datasheet in
`C:/Users/thomas/Downloads/DW3000 Data Sheet.pdf`, page 9, specifies an
active-low reset driven only by an external open-drain output. The original
startup never released the CubeMX PC1-low state. Live SPI reads completed
without HAL errors but returned device ID 0x00000000 while reset was held.

## Maximum TX-power test

On November 11 the drone's requested TX_POWER was changed from FDFDFDFD to
FFFFFFFF at the user's request. Each 0xFF byte encodes fine gain 63 and coarse
gain 3, applied to all four packet sections. PGdly stays 0x34 and PGcount stays
zero; channel, preamble, bitrate, packet format and RF2 selection stay the same.
The existing `UWB CFG` readback check automatically expects PWR=FFFFFFFF now.

This is the maximum raw gain-register setting, not a calibrated radiated-power
level. Final antenna/module output and compliance require RF measurement.
The former FD byte already used fine gain 63 and coarse gain 1. Qorvo's driver
gain table estimates about 1.8 dB more from coarse 1 to 3, so this setting alone
does not explain or promise a large improvement from the observed one-metre link.

The supplied F103 reference folder was not changed. To raise reply power too,
the beacon owner can set the power field of their working code to:

```c
static dwt_txconfig_t txconfig = { 0x34, 0xFFFFFFFFUL, 0 };
```

Both boards need the respective firmware flashed. A transmit-power change does
not require changing the peer's channel/bitrate settings. Increasing just the
drone's power helps the poll direction, not the beacon's reply power.

Encoding/gain reference: Qorvo's `dw3000_txpower_adjustment.c` in Google's UWB
driver tree, commit 713ccd47900ecb22590e580f9407a5c9479b3062, and DW3000
datasheet section 3.11.2.
