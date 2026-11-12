#include "camera_sccb.h"

#define SCL GPIO_PIN_8
#define SDA GPIO_PIN_9
uint32_t camera_sccb_nacks;

static void DelayUs(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = (SystemCoreClock / 1000000U) * us;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) { __NOP(); }
}

static void Data(bool high)
{
    GPIOB->BSRR = high ? SDA : ((uint32_t)SDA << 16);
}

static void ClockLow(void) { GPIOB->BSRR = (uint32_t)SCL << 16; }

static bool ClockHigh(void)
{
    GPIOB->BSRR = SCL;
    uint32_t start = DWT->CYCCNT;
    while (!(GPIOB->IDR & SCL))
        if ((uint32_t)(DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * 200U)
            return false;
    return true;
}

static bool Start(void)
{
    Data(true);
    if (!ClockHigh()) return false;
    DelayUs(10);
    if (!(GPIOB->IDR & SDA)) return false;
    Data(false); DelayUs(10); ClockLow(); DelayUs(10);
    return true;
}

static void Stop(void)
{
    ClockLow(); Data(false); DelayUs(10);
    (void)ClockHigh(); DelayUs(10); Data(true); DelayUs(10);
}

static bool Send(uint8_t byte)
{
    for (unsigned bit = 0; bit < 8; ++bit) {
        Data((byte & 0x80U) != 0); DelayUs(10);
        if (!ClockHigh()) return false;
        DelayUs(10); ClockLow(); DelayUs(10); byte <<= 1;
    }
    /* SCCB's ninth bit is don't-care. Observe it for diagnostics without
     * treating it as I2C arbitration or demanding an ACK on every phase. */
    Data(true); DelayUs(10);
    if (!ClockHigh()) return false;
    DelayUs(10);
    if (GPIOB->IDR & SDA) ++camera_sccb_nacks;
    ClockLow(); DelayUs(10);
    return true;
}

static bool Receive(uint8_t *byte)
{
    uint8_t data = 0;
    Data(true);
    for (unsigned bit = 0; bit < 8; ++bit) {
        DelayUs(10);
        if (!ClockHigh()) return false;
        DelayUs(10);
        data = (uint8_t)((data << 1) | ((GPIOB->IDR & SDA) != 0));
        ClockLow(); DelayUs(10);
    }
    /* Host sends NA after the single data byte. */
    if (!ClockHigh()) return false;
    DelayUs(10); ClockLow(); DelayUs(10);
    *byte = data;
    return true;
}

bool CameraSCCB_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t before = DWT->CYCCNT;
    for (volatile unsigned i = 0; i < 128; ++i) __NOP();
    if (DWT->CYCCNT == before) return false;
    GPIO_InitTypeDef pins = {0};
    GPIOB->BSRR = SCL | SDA;
    pins.Pin = SCL | SDA;
    pins.Mode = GPIO_MODE_OUTPUT_OD;
    pins.Pull = GPIO_NOPULL;
    pins.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &pins);
    camera_sccb_nacks = 0;
    return true;
}

HAL_StatusTypeDef CameraSCCB_Register(uint8_t reg, uint8_t *value, bool read)
{
    bool ok = Start() && Send(0x60) && Send(reg);
    if (!ok) { Stop(); return HAL_TIMEOUT; }
    if (read) {
        Stop(); DelayUs(10);
        ok = Start() && Send(0x61) && Receive(value);
    } else ok = Send(*value);
    Stop();
    return ok ? HAL_OK : HAL_TIMEOUT;
}
