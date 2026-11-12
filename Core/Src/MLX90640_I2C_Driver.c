/* MLX90640_I2C_Driver.c
 * STM32 HAL port of Melexis' platform I2C layer, using I2C3.
 * Match the prototypes/return types to the MLX90640_I2C_Driver.h in YOUR copy
 * of the Melexis library (some versions have void GeneralReset, some int).
 */
#include "MLX90640_I2C_Driver.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;

void MLX90640_I2CInit(void)
{
    /* I2C1 is already initialised by MX_I2C1_Init() */
}

int MLX90640_I2CGeneralReset(void)
{
    return 0; /* not needed for this test */
}

int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress,
                     uint16_t nMemAddressRead, uint16_t *data)
{
    static uint8_t buf[2 * 832];                 /* largest read = 832 words */

    if (nMemAddressRead > 832) {
        return -1;
    }
    if (HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(slaveAddr << 1), startAddress,
                         I2C_MEMADD_SIZE_16BIT, buf,
                         (uint16_t)(nMemAddressRead * 2), 1000) != HAL_OK) {
        return -1;
    }
    for (uint16_t i = 0; i < nMemAddressRead; i++) {
        data[i] = (uint16_t)((buf[2 * i] << 8) | buf[2 * i + 1]);
    }
    return 0;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t d[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };

    if (HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(slaveAddr << 1), writeAddress,
                          I2C_MEMADD_SIZE_16BIT, d, 2, 100) != HAL_OK) {
        return -1;
    }
    return 0;
}

void MLX90640_I2CFreqSet(int freq)
{
    (void)freq;  /* bus speed is set in CubeMX */
}
