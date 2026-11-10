#ifndef UWB_PORT_H
#define UWB_PORT_H

/* Platform glue between the Qorvo DW3000-family driver and STM32F103 HAL.
 *
 * Wiring assumed (change here if yours differs):
 *   SPI2  PB13 SCK, PB14 MISO, PB15 MOSI   (configured by CubeMX as hspi2)
 *   PB12  chip select  (this file configures it, no CubeMX GPIO setup needed)
 *   PB10   RSTn         (open drain, this file configures it)
 * SPI2 is on APB1 (36 MHz), SPI1 on APB2 (72 MHz): the prescalers differ.
 */
#include <stdint.h>

#ifndef UWB_SPI_HANDLE
#define UWB_SPI_HANDLE      hspi2                          /* CubeMX handle name */
#define UWB_SPI_PRESC_SLOW  SPI_BAUDRATEPRESCALER_8        /* 36/16 = 2.25 MHz */
#define UWB_SPI_PRESC_FAST  SPI_BAUDRATEPRESCALER_2        /* 36/2 = 18 MHz  */
/* If you move to SPI1 (72 MHz bus): use hspi1, PRESCALER_16 (4.5) and PRESCALER_4 (18). */
#endif
#ifndef UWB_CS_PORT
#define UWB_CS_PORT   GPIOB
#define UWB_CS_PIN    GPIO_PIN_12
#endif
#ifndef UWB_RST_PORT
#define UWB_RST_PORT  GPIOB
#define UWB_RST_PIN   GPIO_PIN_10
#endif

void uwb_port_init(void);   /* GPIO + microsecond timer. Call once after MX_SPI2_Init(). */
void uwb_spi_slow(void);    /* 2.25 MHz: use until the chip has finished initialising (set prescalar to 16) */
void uwb_spi_fast(void);    /* 18 MHz: normal operation                               */
void uwb_reset(void);       /* pulse RSTn low, then release it
                        */
//additional initial declaration to help bridging the gap upon compilation
int32_t  readfromspi(uint16_t headerLength, uint8_t *headerBuffer,
                 uint16_t readLength, uint8_t *readBuffer);

int32_t  writetospi(uint16_t headerLength, const uint8_t *headerBuffer,
                uint16_t bodyLength, const uint8_t *bodyBuffer);

int32_t  writetospiwithcrc(uint16_t headerLength, const uint8_t *headerBuffer,
                       uint16_t bodyLength, const uint8_t *bodyBuffer, uint8_t crc8);

void port_set_dw_ic_spi_slowrate(void);
void port_set_dw_ic_spi_fastrate(void);
void wakeup_device_with_io(void);
/* The Qorvo driver itself expects these names (readfromspi, writetospi,
 * writetospiwithcrc, deca_sleep, deca_usleep, decamutexon/off); they are
 * defined in uwb_port.c and deliberately NOT declared here, because the
 * Qorvo headers already declare them. */

#endif
