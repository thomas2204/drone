/* MLX90640_I2C_Driver.c
 * STM32 HAL port of Melexis' platform I2C layer, using shared I2C1 PB8/PB9.
 * Match the prototypes/return types to the MLX90640_I2C_Driver.h in YOUR copy
 * of the Melexis library (some versions have void GeneralReset, some int).
 */
#include "MLX90640_I2C_Driver.h"
#include "main.h"
#include "i2c.h"
#include "cmsis_os2.h"
#include "record_control.h"

extern I2C_HandleTypeDef hi2c1;
extern osMutexId_t i2c1MutexHandle;
static osSemaphoreId_t i2c_done;
static volatile uint8_t i2c_err;
static volatile uint8_t thermal_waiting;

void MLX90640_I2CInit(void)
{
    /* I2C1 is already initialised by MX_I2C1_Init() */
    i2c_done = osSemaphoreNew(1, 0, NULL); 
}

int MLX90640_I2CGeneralReset(void)
{
    return 0; /* not needed for this test */
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *h)
{
    if (h == &hi2c1 && thermal_waiting) osSemaphoreRelease(i2c_done);
}
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h)
{
    if (h == &hi2c1 && thermal_waiting) { i2c_err = 1; osSemaphoreRelease(i2c_done); }
}

#define CHUNK_WORDS 127
static uint8_t rx_buf[CHUNK_WORDS * 2] __attribute__((section(".dma_buffer"),aligned(32)));
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress,
                     uint16_t nMemAddressRead, uint16_t *data)
{
     uint16_t done = 0;
     if (startAddress==0x8000 && record_report.active && record_report.started &&
        !RecordControl_HasTime()) return -1;
    while (done < nMemAddressRead) {
        uint16_t n = nMemAddressRead - done;
        if (n > CHUNK_WORDS) n = CHUNK_WORDS;

        osMutexAcquire(i2c1MutexHandle, osWaitForever);
        i2c_err = 0;
        while (osSemaphoreAcquire(i2c_done, 0) == osOK) { }   /* clear stale */
        thermal_waiting = 1;
        HAL_StatusTypeDef st = HAL_I2C_Mem_Read_DMA(&hi2c1, (uint16_t)(slaveAddr << 1),
                                   startAddress + done, I2C_MEMADD_SIZE_16BIT,
                                   rx_buf, n * 2);
        osStatus_t w = osErrorTimeout;
        if (st == HAL_OK) w = osSemaphoreAcquire(i2c_done, 200);   /* task sleeps here */
        thermal_waiting = 0;
        if (st != HAL_OK || w != osOK || i2c_err) {
            HAL_DMA_Abort(hi2c1.hdmarx);   /* stop a stuck transfer */
            HAL_I2C_Init(&hi2c1);          /* returns State to READY */
            osMutexRelease(i2c1MutexHandle);
            return -1;
        }
        osMutexRelease(i2c1MutexHandle);

        if (st != HAL_OK || w != osOK || i2c_err) return -1;
        for (uint16_t i = 0; i < n; i++)
            data[done + i] = (uint16_t)((rx_buf[2*i] << 8) | rx_buf[2*i + 1]);
        done += n;
    }
    /* The vendor API polls status until the next subpage. Yield with the bus
     * released instead of consuming all available CPU and I2C bandwidth. */
    if (startAddress == 0x8000 && nMemAddressRead == 1 && !(data[0] & 0x0008))
        osDelay(2);
    return 0;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t d[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };

    if (osMutexAcquire(i2c1MutexHandle, osWaitForever) != osOK) return -1;
    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(slaveAddr << 1), writeAddress,
                          I2C_MEMADD_SIZE_16BIT, d, 2, 100);
    osMutexRelease(i2c1MutexHandle);
    if (status != HAL_OK) {
        return -1;
    }
    return 0;
}

void MLX90640_I2CFreqSet(int freq)
{
    (void)freq;  /* bus speed is set in CubeMX */
}
