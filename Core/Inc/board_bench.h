#ifndef BOARD_BENCH_H
#define BOARD_BENCH_H

#include <stdint.h>

/* Debugger-visible result: 0=pending, 1=pass, -1=fail.
 * Stage: 1=SDRAM init, 2=memory, 3=finished. */
typedef struct {
    uint32_t stage;
    int32_t sdram_result;
    uint32_t memory_pass;
    uint32_t address;
    uint32_t expected;
    uint32_t actual;
    uint32_t hal_status;
    uint32_t fmc_clock_hz;
    uint32_t sdram_ms;
    uint32_t tested_bytes;
    uint32_t manual_a11;
} BoardBenchReport;

extern volatile BoardBenchReport board_bench;
/* Set while stopped at TestSDRAM to test a bounded region; reset restores
 * the default 32 MiB. This does not configure application buffer limits. */
extern volatile uint32_t board_bench_test_bytes;
/* Diagnostic only: hold A11 as GPIO for each 2 MiB block. This is not normal
 * memory-mapped access and must not be used while another master accesses it. */
extern volatile uint32_t board_bench_manual_a11;
void BoardBench_ConfigureMPU(void);
void BoardBench_Task(void *argument);

#endif
