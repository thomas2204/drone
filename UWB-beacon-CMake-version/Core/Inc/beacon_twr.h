#ifndef BEACON_TWR_H
#define BEACON_TWR_H

#include <stdint.h>

/* UWB beacon = SS-TWR *responder*. The drone (initiator) sends a Poll, the
 * beacon answers with a Response that carries two timestamps, and the drone
 * works out the distance. See beacon_twr.c for the exact message format. */

typedef struct {
    uint32_t polls;          /* valid polls received                           */
    uint32_t replies;        /* responses actually sent                        */
    uint32_t rx_err;         /* receive errors (bad CRC, preamble timeout...)  */
    uint32_t tx_err;         /* delayed TX refused or never completed          */
    uint8_t  last_seq;       /* sequence number of the last poll               */
    uint32_t last_poll_rx;   /* low 32 bits of the last poll RX timestamp      */
    uint32_t last_resp_tx;/* low 32 bits of the last response TX timestamp  */
    int16_t  rssi_q8;        /* last poll's received power, 1/256 dB steps */
    uint8_t  rssi_valid;     /* 1 if rssi_q8 was read successfully         */
} beacon_stats_t;

/* 0 = OK, negative = failed (-1 no idle, -2 init, -3 configure). */
int  beacon_twr_init(void);

/* Call as often as you like from the main loop; it never blocks for long. */
void beacon_twr_step(void);

const beacon_stats_t *beacon_twr_stats(void);

#endif
