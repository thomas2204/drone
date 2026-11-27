# RC7 Guided test

Build the STM32 companion with `cmake --preset Debug` and
`cmake --build --preset Debug`. In VS Code, the **Drone + MP4 camera
(Debug)** launch entry loads the resulting firmware. The separate
`DebugRc7Guided` preset also enables the sequence.

In Mission Planner, set `RC7_OPTION=0` to release RC7 from the EKF source
switch. Keep PX4Flow configured in source set 1. Assign a three-position
transmitter switch to RC7 and keep a separate flight mode switch for pilot
takeover. The STM32 reads RC7 over MAVLink and waits for confirmation that
`RC7_OPTION=0`.

- **Low** establishes the starting position for the switch. During the
  sequence it requests Land.
- **Mid** starts takeoff once the pilot has armed manually and selected Guided.
  If transmitter arming is unavailable in Guided, arm in a pilot-controlled
  mode and select Guided while still on the ground.
  Start on level ground with the TFmini reading 10 to 35 cm. Hold Low until
  the controller has at least one second of healthy, settled telemetry
  with estimated vertical speed below 0.20 m/s in either direction.
  Keep the switch at Mid. With `WP_RFND_USE=1` and healthy rangefinder
  terrain data, the takeoff command targets 50 cm above the floor. For
  example, a starting TFmini reading of 15 cm gives a 35 cm climb.
  Guided stays in takeoff until the TFmini and EKF meet the steady hover
  conditions below for one second. The companion then holds a fixed
  vertical position target.
  It requests Land if takeoff has not completed within 12 seconds.
  If the aircraft settles between 30 and 40 cm after confirmed liftoff,
  the companion holds the horizontal position and raises the altitude
  setpoint in steps of at most 1.5 cm, up to 20 cm total. It stops raising
  the setpoint at a measured 48 cm. It requests Land if the adjustment
  does not settle within eight seconds or leaves the 30–60 cm range.
  After one steady second within 10 cm of target, with the range
  staying within 3 cm of its value at the start of that second and the
  EKF reporting low speed, it moves 1 m along the current heading.
  After settling at the endpoint, it returns to the horizontal position
  saved at takeoff, keeping the same heading and hover altitude. It then
  holds that position. If the hover does not settle within eight
  seconds of entering hover, it requests Land.
- **High** cancels the pending forward move or stops either leg and holds the
  current position. Returning to Mid keeps that hold.

The feature uses a relative EKF position, fresh RC and flight telemetry,
and a valid downward laser range. ArduPilot also needs an EKF origin for
non-GPS position modes. Switching out of Guided gives control
back to the pilot and ends the sequence. The companion stops issuing
targets on RC or heartbeat loss. When the link is healthy and the
rangefinder or navigation estimate fails during the sequence, it requests
Land. It also requests Land if the TFmini shows a rise of over 85 cm
from the starting reading or if the EKF and TFmini height changes differ
by more than 40 cm. A move that
leaves the hover band drops back to holding position. The flight controller
retains its own arming checks and failsafes.

Use the simulator first. The strip shows blue during the forward leg and
pink during the return. Green indicates ready to arm or armed outside
those two legs.
