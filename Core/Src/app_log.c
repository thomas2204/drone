#include "app_log.h"
#include "cmsis_os2.h"
#include "usart.h"
static osMutexId_t log_mutex;
bool AppLog_Init(void)
{
    const osMutexAttr_t attr={.name="bluetooth_tx",.attr_bits=osMutexPrioInherit};
    if (!log_mutex) log_mutex=osMutexNew(&attr);
    return log_mutex!=NULL;
}
HAL_StatusTypeDef AppLog_Write(const void *data,uint16_t bytes,uint32_t timeout)
{
    if (!log_mutex || osMutexAcquire(log_mutex,osWaitForever)!=osOK)return HAL_ERROR;
    HAL_StatusTypeDef result=HAL_UART_Transmit(&huart7,(const uint8_t *)data,bytes,timeout);
    osMutexRelease(log_mutex);return result;
}
