# Bluetooth rectangle patrol

`patrol` requests normal Guided mode, normal arming and takeoff, then a
10 m forward / 5 m right / 10 m back / 5 m left rectangle. It stops at each
corner and turns right 90 degrees, including the final turn back to the launch
heading. It finishes hovering over the launch corner. The rectangle orientation
is fixed from the heading and EKF local position when the command is accepted.
No custom FC mode is required.

## Height reference

The downward TFmini is connected to the FC. The companion requests the FC's
`DISTANCE_SENSOR` messages and accepts only a downward laser (orientation 25).
It tilt-corrects distance with cos(roll)*cos(pitch). The target is **3 m above
the surface seen by that sensor**, not GPS MSL altitude. Takeoff uses the FC's
normal above-home takeoff command, reduced by the initial sensor ground
clearance. Horizontal motion cannot begin until the measured height is near
3 m and vertical motion has settled. During the rectangle and final hover,
ToF height error supplies bounded corrections to the local NED z target.
The FC retains its normal inner flight-control loops and configured EKF sources.

This is intended for an open, flat, unobstructed area. A single downward sensor
cannot detect obstacles ahead. Changes in the surface under the sensor can
change the commanded altitude. Ground clearance above 1 m rejects starting.

The previous saved FC configuration reported RNGFND1_TYPE=20, ORIENT=25,
MIN=0.1 m and MAX=10 m. These values were not changed by this implementation.
Current live sensor health, orientation, range and reception still need checking.

## Commands

- `patrol status`: print the current state/rejection reason; no arming or flight.
- `patrol`: start once, only while disarmed/on ground and all required telemetry
  and checks have been continuously healthy for ten seconds. A rejected request
  is discarded; it cannot launch later merely because checks improve.
- `patrol stop`: cancel the rectangle and hold the current XY location at the
  requested 3 m ToF height. Before arming it cancels starting; during arming it
  requests Land if already armed. It does not cut motors in flight.
- `patrol land`: cancel the sequence and request the FC's normal Land mode.

Commands are case insensitive and accept the terminal's normal newline or the
existing 150 ms idle delimiter. Existing `record` camera commands remain usable.
Bluetooth prints patrol state changes. A second `patrol` never restarts an
active sequence. Reboot does not start/resume a patrol.

## Checks and cancellation

Requires fresh heartbeat, SYS_STATUS, attitude, local position, EKF status,
landed-state and range messages. Disabled/unavailable pre-arm checks do not
qualify as success. The controller checks absolute horizontal/vertical EKF
position, velocity, GPS-glitch/uninitialized flags, enabled sensor health, and
TFmini data validity. It sends no forced-arm command or origin/parameter writes.

Any FC/pilot mode change ends the sequence without attempting to reclaim
Guided. If navigation/ToF fails while armed in Guided, the controller requests
Land rather than continuing the rectangle. If telemetry is stale it stops
issuing movement targets, latches a landing state and requests Land if the link
returns still in Guided. FC-native failsafes retain authority while disconnected.
It does not disarm motors in flight. Takeoff/leg/turn deadlines and distance/
height bounds also request Land. A completely frozen companion cannot issue
new commands; FC configuration remains responsible for its own failsafe behavior.

Bluetooth link loss itself cannot be detected with the current wiring, so it
does not cancel the autonomous rectangle. RC mode selection remains the primary
independent pilot takeover. FC position/yaw targets are sent at 5 Hz from the
same UART-owning task as GPS forwarding. Intermediate XY targets advance at no
more than 0.5 m/s; this is a setpoint pacing bound, not a guarantee of actual
airspeed or obstacle clearance. GPS error can still move the real path by metres.

## Validation and deployment

- `tests/test_patrol.c`: production state machine, rectangle closure, ToF z
  direction, hover, qualification, mode takeover, stop/land, stale link, rejected
  takeoff, deadlines and clock wrap.
- `tests/test_record_control.py`: actual Bluetooth parser, patrol commands and
  overflow rejection, plus existing recording/session tests.
- `tests/patrol_sitl.py`: production state machine against a separately spawned
  ArduCopter SITL on localhost only, including arming, takeoff, four corners and
  a five-second final hover. The range input comes from simulated vehicle truth
  through the simulated FC's MAVLink rangefinder driver, not physical TFmini data.
- Normal CMake Debug ARM build.

These checks do not constitute physical-aircraft validation. With props removed,
first verify `patrol status` and actual downward sensor telemetry. Sending
`patrol` is a flight/arming request, not a harmless status test. Confirm RC mode
takeover and normal FC failsafes before a real flight.

References: [Guided commands](https://ardupilot.org/dev/docs/copter-commands-in-guided-mode.html),
[FC rangefinder telemetry](https://github.com/ArduPilot/ardupilot/blob/9f648cca/libraries/GCS_MAVLink/GCS_Common.cpp#L424).
