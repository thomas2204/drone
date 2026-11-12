#ifndef THERMAL_TEST_H
#define THERMAL_TEST_H

/* Reads the MLX90640, prints temperatures over the serial port, and logs
 * frames to the TF card. Never returns. Call it from main() before the
 * infinite loop, or from the default FreeRTOS task. */
void thermal_test_run(void);

#endif
