/* uwb_port.c : STM32F103 HAL glue for the Qorvo DW3000-family driver.
 *
 * The driver never touches hardware itself. Whenever it needs to talk to the
 * chip it calls the functions below, so this file is the only place that knows
 * about SPI, GPIO and delays.
 */
#include "uwb_port.h"
#include "main.h"                 /* HAL */

extern SPI_HandleTypeDef UWB_SPI_HANDLE;   /* created by CubeMX */

#define SPI_TIMEOUT_MS  10

static inline void cs_low(void)  { HAL_GPIO_WritePin(UWB_CS_PORT, UWB_CS_PIN, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(UWB_CS_PORT, UWB_CS_PIN, GPIO_PIN_SET); }

/* ---- setup ---- */
void uwb_port_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* chip select: push-pull, idle high */
    HAL_GPIO_WritePin(UWB_CS_PORT, UWB_CS_PIN, GPIO_PIN_SET);
    g.Pin   = UWB_CS_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(UWB_CS_PORT, &g);

    /* reset: open drain. Writing 1 releases the line (the chip pulls it up).
     * Never drive RSTn high from the MCU. */
    HAL_GPIO_WritePin(UWB_RST_PORT, UWB_RST_PIN, GPIO_PIN_SET);
    g.Pin   = UWB_RST_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(UWB_RST_PORT, &g);

    /* free-running cycle counter, used for microsecond delays (Cortex-M3 DWT) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static void spi_set_prescaler(uint32_t presc)
{
    __HAL_SPI_DISABLE(&UWB_SPI_HANDLE);
    MODIFY_REG(UWB_SPI_HANDLE.Instance->CR1, SPI_CR1_BR, presc);
    __HAL_SPI_ENABLE(&UWB_SPI_HANDLE);
}

void uwb_spi_slow(void) { spi_set_prescaler(UWB_SPI_PRESC_SLOW); }
void uwb_spi_fast(void) { spi_set_prescaler(UWB_SPI_PRESC_FAST); }

void uwb_reset(void)
{
    HAL_GPIO_WritePin(UWB_RST_PORT, UWB_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(2);
    HAL_GPIO_WritePin(UWB_RST_PORT, UWB_RST_PIN, GPIO_PIN_SET);   /* release */
}

/* ---- names some SDK versions look for ---- */
void port_set_dw_ic_spi_slowrate(void) { uwb_spi_slow(); }
void port_set_dw_ic_spi_fastrate(void) { uwb_spi_fast(); }
void reset_DWIC(void)                  { uwb_reset(); }

void wakeup_device_with_io(void)
{
    cs_low();
    HAL_Delay(1);
    cs_high();
    HAL_Delay(5);
}

/* ---- SPI transfers used by the driver ----
 * Every register access is: chip select low, send a header (register address),
 * then either send the data or read it back, chip select high. */
int32_t writetospi(uint16_t headerLength, const uint8_t *headerBuffer,
               uint16_t bodyLength, const uint8_t *bodyBuffer)
{
    HAL_StatusTypeDef r;

    cs_low();
    r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, (uint8_t *)headerBuffer, headerLength, SPI_TIMEOUT_MS);
    if (r == HAL_OK && bodyLength) {
        r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, (uint8_t *)bodyBuffer, bodyLength, SPI_TIMEOUT_MS);
    }
    cs_high();
    return (r == HAL_OK) ? 0 : -1;
}

int32_t writetospiwithcrc(uint16_t headerLength, const uint8_t *headerBuffer,
                      uint16_t bodyLength, const uint8_t *bodyBuffer, uint8_t crc8)
{
    HAL_StatusTypeDef r;

    cs_low();
    r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, (uint8_t *)headerBuffer, headerLength, SPI_TIMEOUT_MS);
    if (r == HAL_OK && bodyLength) {
        r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, (uint8_t *)bodyBuffer, bodyLength, SPI_TIMEOUT_MS);
    }
    if (r == HAL_OK) {
        r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, &crc8, 1, SPI_TIMEOUT_MS);
    }
    cs_high();
    return (r == HAL_OK) ? 0 : -1;
}

int32_t readfromspi(uint16_t headerLength, uint8_t *headerBuffer,
                uint16_t readLength, uint8_t *readBuffer)
{
    HAL_StatusTypeDef r;

    cs_low();
    r = HAL_SPI_Transmit(&UWB_SPI_HANDLE, headerBuffer, headerLength, SPI_TIMEOUT_MS);
    if (r == HAL_OK && readLength) {
        r = HAL_SPI_Receive(&UWB_SPI_HANDLE, readBuffer, readLength, SPI_TIMEOUT_MS);
    }
    cs_high();
    return (r == HAL_OK) ? 0 : -1;
}

/* ---- delays ---- */
void deca_sleep(unsigned int time_ms) { HAL_Delay(time_ms); }

void deca_usleep(unsigned long time_us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = (uint32_t)time_us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) { }
}

/* ---- critical section hooks ----
 * This beacon polls the chip and never uses its IRQ pin, so nothing can
 * interrupt a register access and these can stay empty. */
int  decamutexon(void)       { return 0; }
void decamutexoff(int state) { (void)state; }
