# RC7 Guided test

Build the STM32 companion with `cmake --preset DebugRc7Guided` and
`cmake --build --preset DebugRc7Guided`. The normal Debug preset keeps these
switch actions disabled.

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
  Keep the switch at Mid. The companion targets 50 cm measured by the
  TFmini. After one steady second within 10 cm of target, with the range
  staying within 3 cm of its value at the start of that second and the
  EKF reporting low speed, it moves 1 m along the current heading once
  and holds the endpoint. If the hover does not settle within eight
  seconds of confirmed liftoff, it requests Land.
- **High** cancels the pending forward move or stops it and holds the
  current position. Returning to Mid keeps that hold.

The feature uses a relative EKF position, fresh RC and flight telemetry,
and a valid downward laser range. ArduPilot also needs an EKF origin for
non-GPS position modes. Switching out of Guided gives control
back to the pilot and ends the sequence. The companion stops issuing
targets on RC or heartbeat loss. When the link is healthy and the
rangefinder or navigation estimate fails during the sequence, it requests
Land. It also requests Land if the TFmini reads over 85 cm. A move that
leaves the hover band drops back to holding position. The flight controller
retains its own arming checks and failsafes.

Use the simulator first. A green strip indicates arming status. It does
not verify the surface along the path or obstacle clearance.
