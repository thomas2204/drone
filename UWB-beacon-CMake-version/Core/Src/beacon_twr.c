/* beacon_twr.c : SS-TWR responder for a DWM3220 on an STM32F103.
 *
 * 			AI-provided info
 * How single-sided two-way ranging works
 * --------------------------------------
 *   drone                         beacon (this code)
 *     |--- Poll ------------------>|  beacon stamps poll_rx_ts (its own clock)
 *     |                            |  waits a fixed delay, then sends:
 *     |<-- Response ---------------|  {poll_rx_ts, resp_tx_ts} inside the frame
 *   drone stamps poll_tx_ts, resp_rx_ts on ITS clock and computes:
 *
 *     rtd_init = resp_rx_ts - poll_tx_ts          (round trip seen by drone)
 *     rtd_resp = resp_tx_ts - poll_rx_ts          (beacon's reply time, from frame)
 *     tof      = (rtd_init - rtd_resp * (1 - clockOffsetRatio)) / 2
 *     distance = tof * speed_of_light
 *
 *   clockOffsetRatio = dwt_readclockoffset() / 2^26, read by the drone right
 *   after receiving the Response. Without it, the two crystals disagree and the
 *   error grows with the reply delay.
 *
 * Message formats (same layout as Qorvo's ex_06 SS-TWR example, so the drone
 * can start from ex_06a). The last 2 bytes of each frame are the CRC, which
 * the chip fills in by itself.
 *
 *   Poll     12 B: 41 88 SN 9A 60 'W' 'A' 'V' 'E' E0 00 00
 *   Response 20 B: 41 88 SN 9A 60 'V' 'E' 'W' 'A' E1 | poll_rx_ts(4, LE) | resp_tx_ts(4, LE) | CRC(2)
 *
 *   SN = sequence number; the beacon copies it from the poll into the response.
 *   Timestamps are the low 32 bits of the chip's 40-bit counter (15.65 ps units).
 *
 * IMPORTANT: this file uses the Qorvo DW3000-family driver API (dwt_*). Function
 * names and signatures differ slightly between SDK versions. Everything that
 * touches the driver is in this file; if your SDK complains, compare with the
 * ss_twr_resp (ex_06b) example that ships with it.
 */
#include "beacon_twr.h"
#include "uwb_port.h"
#include "main.h"
#include "deca_device_api.h"
#include "dw3000_deca_regs.h"
#include <string.h>
#include <stdio.h>

/* Set to 1 if your SDK's deca_device_api.h declares dwt_probe(). Then also add
 * the SDK's deca_probe_interface.c to the build (it defines dw3000_probe_interf). */
#ifndef UWB_USE_PROBE
#define UWB_USE_PROBE 1
#endif
#if UWB_USE_PROBE
extern const struct dwt_probe_s dw3000_probe_interf;
#endif

/* ---- radio settings: the drone MUST use exactly the same ones ---- */
static dwt_config_t config = {
    5,                  /* channel                                            */
    DWT_PLEN_1024,      /* preamble length (TX)                               */
    DWT_PAC32,          /* preamble acquisition chunk (RX)                    */
    9,                  /* TX preamble code                                   */
    9,                  /* RX preamble code                                   */
    2,                  /* SFD type: 16 symbols                               */
    DWT_BR_850K,         /* data rate 850 kbps                                 */
    DWT_PHRMODE_STD,    /* standard PHY header                                */
    DWT_PHRRATE_STD,
    (1024 + 1 + 16 - 32), /* SFD timeout: 1009 symbols                         */
    DWT_STS_MODE_OFF,   /* no STS for now. Turn on together with the drone's  */
    DWT_STS_LEN_64,     /* PDoA mode later.                                   */
    DWT_PDOA_M0
};

/* Generic DW3000 channel-5 values from Qorvo's examples. For best range and to
 * stay within regulations, use the values recommended for the DWM3220 module. */
static dwt_txconfig_t txconfig = { 0x34, 0xffffffff, 0x0 };



/* Antenna delay, in chip time units. 16385 is Qorvo's default. Calibrating it
 * per board is what makes distances accurate (1 ns of error = 30 cm). */
#define TX_ANT_DLY  16385
#define RX_ANT_DLY  16385

/* Time between receiving the poll and sending the response, in microseconds.
 * Must be long enough for the MCU to read the poll and program the reply
 * (a few hundred us on an F103). The drone must wait longer than this for it. */
#define POLL_RX_TO_RESP_TX_DLY_US  2500
#define US_TO_DWT_TIME             63898ULL     /* chip time units per microsecond */

/* ---- message layout ---- */
#define MSG_COMMON_LEN       10
#define MSG_SN_IDX           2
#define RESP_POLL_RX_TS_IDX  10
#define RESP_RESP_TX_TS_IDX  14
#define RESP_TS_LEN          4

static const uint8_t rx_poll_msg[] = { 0x41, 0x88, 0, 0x9A, 0x60, 'W', 'A', 'V', 'E', 0xE0, 0, 0 };
static uint8_t       tx_resp_msg[] = { 0x41, 0x88, 0, 0x9A, 0x60, 'V', 'E', 'W', 'A', 0xE1,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static uint8_t        rx_buf[32];
static int            rx_armed;
static beacon_stats_t st;

/* ---- helpers ---- */
static uint64_t ts_to_u64(const uint8_t *t)        /* 5 bytes, little endian */
{
    uint64_t v = 0;
    for (int i = 4; i >= 0; i--) {
        v = (v << 8) | t[i];
    }
    return v;
}

static void put_ts32(uint8_t *dst, uint64_t ts)    /* low 32 bits, little endian */
{
    for (int i = 0; i < RESP_TS_LEN; i++) {
        dst[i] = (uint8_t)ts;
        ts >>= 8;
    }
}

const beacon_stats_t *beacon_twr_stats(void) { return &st; }

/* ---- init ---- */
int beacon_twr_init(void)
{
    uint32_t t0;

    printf("1. port init\r\n");
    uwb_port_init();
    uwb_spi_slow();    /* slow clock until the chip is initialised */
    printf("2. reset\r\n");
    uwb_reset();
    deca_sleep(2);                  /* chip start-up time */

#if UWB_USE_PROBE
    printf("3. probe\r\n");
    dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);
#endif
    printf("4. waiting IDLE_RC\r\n");
    t0 = HAL_GetTick();
    while (!dwt_checkidlerc()) {    /* wait for IDLE_RC, give up after 100 ms */
        if (HAL_GetTick() - t0 > 100) {
        	printf("TIMEOUT waiting IDLE_RC\r\n");
            return -1;
        }
    }
    printf("5. initialise\r\n");
    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
        return -2;
    }
    printf("6. configure\r\n");
    uwb_spi_fast();

    if (dwt_configure(&config)) {   /* non-zero = PLL or RX calibration failed */
    	printf("dwt_configure FAILED\r\n");
        return -3;
    }
    dwt_configuretxrf(&txconfig);
    dwt_configure_rf_port(DWT_RF_PORT_MANUAL_1);
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    rx_armed = 0;
    memset(&st, 0, sizeof st);
    printf("init OK\r\n");
    return 0;
}

/* ---- one pass of the responder state machine ---- */
void beacon_twr_step(void)
{
    uint32_t status, len;

    if (!rx_armed) {                          /* listen for the next poll */
        dwt_setrxtimeout(0);                  /* 0 = wait forever */
        dwt_rxenable(DWT_START_RX_IMMEDIATE);
        rx_armed = 1;
    }

    status = dwt_readsysstatuslo();
    if (!(status & (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR))) {
        return;                               /* nothing yet, come back later */
    }
    rx_armed = 0;                             /* the receiver stops after any frame/error */

    if (!(status & SYS_STATUS_RXFCG_BIT_MASK)) {
    	dwt_forcetrxoff();
		dwt_writesysstatuslo(SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO);   /* clear error flags */
        st.rx_err++;
        return;
    }

    /* good frame */
    dwt_write_reg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    len = dwt_read_reg(RX_FINFO_ID) & 0x3FFU;         /* frame length incl. CRC */
    if (len != sizeof(rx_poll_msg) || len > sizeof(rx_buf)) {
        return;                               /* not our poll */
    }
    dwt_readrxdata(rx_buf, (uint16_t)len, 0);

    uint8_t seq = rx_buf[MSG_SN_IDX];
    rx_buf[MSG_SN_IDX] = 0;                   /* ignore SN when comparing */
    if (memcmp(rx_buf, rx_poll_msg, MSG_COMMON_LEN) != 0) {
        return;                               /* some other device's frame */
    }
    st.polls++;
    st.last_seq = seq;

    /* 1. when did the poll arrive, on our clock? */
    uint8_t  ts_raw[5];
    dwt_readrxtimestamp(ts_raw, IPATOV);
    uint64_t poll_rx_ts = ts_to_u64(ts_raw);

    /* 2. schedule the response a fixed time later. The chip ignores the lowest
     *    9 bits of the send time, hence the >> 8 and the & 0xFFFFFFFE. */
    uint32_t resp_tx_time = (uint32_t)((poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_US * US_TO_DWT_TIME)) >> 8);
    dwt_setdelayedtrxtime(resp_tx_time);

    /* the real TX timestamp is that scheduled time plus our antenna delay */
    uint64_t resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

    /* 3. build and send the response with both timestamps inside */
    put_ts32(&tx_resp_msg[RESP_POLL_RX_TS_IDX], poll_rx_ts);
    put_ts32(&tx_resp_msg[RESP_RESP_TX_TS_IDX], resp_tx_ts);
    tx_resp_msg[MSG_SN_IDX] = seq;
    dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
    dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);          /* offset 0, ranging frame */

    if (dwt_starttx(DWT_START_TX_DELAYED) != DWT_SUCCESS) {
        st.tx_err++;                          /* too late: the scheduled time had passed */
        return;
    }

    uint32_t t0 = HAL_GetTick();
    while (!(dwt_read_reg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK)) {
        if (HAL_GetTick() - t0 > 10) {
            st.tx_err++;
            return;
        }
    }
    dwt_write_reg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);

    st.replies++;
    st.last_poll_rx = (uint32_t)poll_rx_ts;
    st.last_resp_tx = (uint32_t)resp_tx_ts;
    /* received power of the poll: read AFTER replying, so it can't delay the response */
    {
        dwt_cirdiags_t diagnostics;
        st.rssi_valid =
            (dwt_readdiagnostics_acc(&diagnostics, DWT_ACC_IDX_IP_M) == DWT_SUCCESS) &&
            (dwt_calculate_rssi(&diagnostics, DWT_ACC_IDX_IP_M, &st.rssi_q8) == DWT_SUCCESS);
    }
}
