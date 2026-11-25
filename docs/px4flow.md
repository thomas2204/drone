# PX4Flow on the JHEMCU-GF30F745

The flight-tested PX4Flow v1.3.1 sends MAVLink directly to the flight
controller. PX4Flow TX connects to JHEMCU RX4, which is `SERIAL4` on the
custom board. The link runs at 115200 baud. The STM32 companion build uses
its normal settings, with `CAMERA_FLOW_MAVLINK` off, so the PX4Flow is the
sole optical-flow publisher.

## Flight controller build

The tested ArduCopter revision was `9f648cca`. From the existing ArduPilot
checkout with the custom JHEMCU-GF30F745 board definition, use the
[PX4Flow hardware overlay](../config/ardupilot/JHEMCU-GF30F745/px4flow.hwdef)
with Waf. Set `DRONE_REPO` to the absolute path of this repository.

```sh
./waf configure --board JHEMCU-GF30F745 --extra-hwdef "$DRONE_REPO/config/ardupilot/JHEMCU-GF30F745/px4flow.hwdef"
./waf copter
```

## Parameters and verification

The [PX4Flow parameter file](../config/ardupilot/JHEMCU-GF30F745/px4flow.param)
contains the values recorded in the Loiter flight log for this installation.
Compare it against a saved parameter export in Mission Planner before
writing. The +131° yaw and scale corrections belong to this particular
mount and sensor.

The tested setup uses the downward TFmini for surface tracking and the DPS310
as primary barometer (`BARO_PRIMARY=0`). Confirm the TFmini reports a valid
range above its configured minimum and that the EKF has a valid origin.
With the quad disarmed and props removed, select **Data → Aux Function → EKF
Source Set → Mid** after boot. Check for a fresh `EKF3 IMU0 fusing optical flow`
message and relative position telemetry.

The Loiter test verified optical-flow fusion. The aircraft's companion
patrol code still requires an absolute horizontal EKF position estimate,
and the flight log recorded a low-battery failsafe. Those checks matter
before testing an autonomous patrol.
