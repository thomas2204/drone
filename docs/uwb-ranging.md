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
