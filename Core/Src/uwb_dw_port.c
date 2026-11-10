/* STM32H743 polling SPI5 transport. No DMA/cache maintenance is required.
 * Board: PF7 SCK, PF8 MISO, PF9 MOSI, PF10 CS. IRQ is unused.
 * DWM1 RSTn (pad 3) is wired to PC1, an open-drain output. */
#include "main.h"
#include "spi.h"
#include "cmsis_os2.h"
#include "deca_interface.h"

static void select_chip(GPIO_PinState state)
{
    HAL_GPIO_WritePin(UWB_CS_GPIO_Port, UWB_CS_Pin, state);
}

static void set_rate(uint32_t divider)
{
    __HAL_SPI_DISABLE(&hspi5);
    MODIFY_REG(hspi5.Instance->CFG1, SPI_CFG1_MBR, divider);
    hspi5.Init.BaudRatePrescaler = divider;
    /* HAL enables the peripheral at the start of each transaction. */
}

/* SPI5 runs from APB1. Its kernel is now 100 MHz, not the old 32 MHz.
 * Stay under 2 MHz at startup and 16 MHz in normal operation. */
static void select_rate(uint32_t maximum)
{
    static const uint32_t rates[] = {
        SPI_BAUDRATEPRESCALER_2, SPI_BAUDRATEPRESCALER_4,
        SPI_BAUDRATEPRESCALER_8, SPI_BAUDRATEPRESCALER_16,
        SPI_BAUDRATEPRESCALER_32, SPI_BAUDRATEPRESCALER_64,
        SPI_BAUDRATEPRESCALER_128, SPI_BAUDRATEPRESCALER_256
    };
    uint32_t clock = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI5);
    unsigned i = 0;
    while (i < 7U && (clock == 0U || clock / (2U << i) > maximum)) ++i;
    set_rate(rates[i]);
}

static void slow_rate(void) { select_rate(2000000U); }
static void fast_rate(void) { select_rate(16000000U); }

static int32_t spi_write(uint16_t hn, const uint8_t *header,
                         uint16_t n, const uint8_t *body)
{
    select_chip(GPIO_PIN_RESET);
    HAL_StatusTypeDef r = HAL_SPI_Transmit(&hspi5, (uint8_t *)header, hn, 10);
    if (r == HAL_OK && n != 0)
        r = HAL_SPI_Transmit(&hspi5, (uint8_t *)body, n, 10);
    select_chip(GPIO_PIN_SET);
    return r == HAL_OK ? 0 : -1;
}

static int32_t spi_write_crc(uint16_t hn, const uint8_t *header,
                             uint16_t n, const uint8_t *body, uint8_t crc)
{
    select_chip(GPIO_PIN_RESET);
    HAL_StatusTypeDef r = HAL_SPI_Transmit(&hspi5, (uint8_t *)header, hn, 10);
    if (r == HAL_OK && n != 0)
        r = HAL_SPI_Transmit(&hspi5, (uint8_t *)body, n, 10);
    if (r == HAL_OK) r = HAL_SPI_Transmit(&hspi5, &crc, 1, 10);
    select_chip(GPIO_PIN_SET);
    return r == HAL_OK ? 0 : -1;
}

static int32_t spi_read(uint16_t hn, uint8_t *header, uint16_t n, uint8_t *body)
{
    select_chip(GPIO_PIN_RESET);
    HAL_StatusTypeDef r = HAL_SPI_Transmit(&hspi5, header, hn, 10);
    if (r == HAL_OK && n != 0) r = HAL_SPI_Receive(&hspi5, body, n, 10);
    select_chip(GPIO_PIN_SET);
    return r == HAL_OK ? 0 : -1;
}

void deca_sleep(unsigned int ms)
{
    if (osKernelGetState() == osKernelRunning)
        osDelay((uint32_t)(((uint64_t)ms * osKernelGetTickFreq() + 999) / 1000) + 1);
    else HAL_Delay(ms);
}

void deca_usleep(unsigned long us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = (uint32_t)us * (SystemCoreClock / 1000000U);
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }
}

/* Only the UWB task accesses this driver; no ISR accesses SPI5. */
decaIrqStatus_t decamutexon(void) { return 0; }
void decamutexoff(decaIrqStatus_t state) { (void)state; }

static void wakeup(void)
{
    select_chip(GPIO_PIN_RESET);
    deca_sleep(1);
    select_chip(GPIO_PIN_SET);
    deca_sleep(5);
}

extern const struct dwt_driver_s dw3000_driver;
static const struct dwt_driver_s *drivers[] = { &dw3000_driver };
static const struct dwt_spi_s spi_ops = {
    .readfromspi = spi_read, .writetospi = spi_write,
    .writetospiwithcrc = spi_write_crc,
    .setslowrate = slow_rate, .setfastrate = fast_rate
};
static struct dwt_probe_s probe = {
    .dw = NULL, .spi = (void *)&spi_ops, .wakeup_device_with_io = wakeup,
    .driver_list = (struct dwt_driver_s **)drivers, .dw_driver_num = 1
};

int UWB_DriverProbe(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55; /* unlock the Cortex-M7 cycle counter */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    /* A stopped cycle counter would make deca_usleep loop forever. */
    uint32_t before = DWT->CYCCNT;
    for (volatile uint32_t i = 0; i < 100U; ++i) { __NOP(); }
    if (DWT->CYCCNT == before) return -6;
    slow_rate();
    /* Schematic DWM1 pad 3 -> PC1. CubeMX initially drives it low.
     * Pulse low then release; never drive the DW3000 reset line high
     * with a push-pull output or an external pull-up. */
    GPIO_InitTypeDef reset = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET);
    reset.Pin = GPIO_PIN_1;
    reset.Mode = GPIO_MODE_OUTPUT_OD;
    reset.Pull = GPIO_NOPULL;
    reset.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &reset);
    deca_sleep(2);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_SET); /* open drain: release */
    deca_sleep(5);
    return dwt_probe(&probe);
}

void UWB_DriverFast(void) { fast_rate(); }
