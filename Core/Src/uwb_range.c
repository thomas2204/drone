/* Initiator matching UWB-beacon-CMake-version/Core/Src/beacon_twr.c.
 * Poll -> response carrying two LE32 timestamps; no final packet. */
#include "uwb_range.h"
#include "uwb_protocol.h"
#include "main.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "deca_device_api.h"
#include "dw3000_deca_regs.h"
#include <string.h>

extern int UWB_DriverProbe(void);
extern void UWB_DriverFast(void);

static UWB_RangeSnapshot shared, current;
static uint8_t sequence;
/* Preserve the port selected for the user's current antenna test. */
static const dwt_rf_port_ctrl_e rf_port = DWT_RF_PORT_MANUAL_2;
/* Match the newly pulled F103 PHY. Its response is scheduled 2500 us after
 * poll RX. Keep a 10000-UUS (~10.26 ms) receive window for the slower frames. */
#define UWB_RX_TIMEOUT_UUS 10000U
#define UWB_EXCHANGE_TIMEOUT_MS 25U
static dwt_config_t config = {
    .chan = 5, .txPreambLength = DWT_PLEN_256, .rxPAC = DWT_PAC16,
    .txCode = 9, .rxCode = 9, .sfdType = 2, /* Qorvo 16-symbol SFD */
    .dataRate = DWT_BR_850K,
    .phrMode = DWT_PHRMODE_STD, .phrRate = DWT_PHRRATE_STD,
    .sfdTO = (256 + 1 + 16 - 16),
    .stsMode = DWT_STS_MODE_OFF, .stsLength = DWT_STS_LEN_64,
    .pdoaMode = DWT_PDOA_M0
};
/* Match the peer's requested power; FE is coarse=2/fine=63, not maximum FF. */
static dwt_txconfig_t tx_rf = { .PGdly = 0x34, .power = 0xFEFEFEFEUL, .PGcount = 0 };


static void publish(void)
{
    taskENTER_CRITICAL();
    shared = current;
    taskEXIT_CRITICAL();
}

void UWB_RangeInit(void)
{
    memset(&current, 0, sizeof current);
    current.init_result = 1;
    current.rf_port = (uint8_t)rf_port;
    /* Before osKernelStart the port's critical nesting is not initialized.
     * A task critical section here leaves BASEPRI masked until scheduling.
     * No other task exists yet, so this assignment needs no lock. */
    shared = current;
}

void UWB_RangeTaskUnavailable(void)
{
    current.init_result = -5;
    shared = current; /* Startup only, before the scheduler. */
}

bool UWB_RangeGetSnapshot(UWB_RangeSnapshot *out)
{
    if (out == NULL) return false;
    taskENTER_CRITICAL();
    *out = shared;
    taskEXIT_CRITICAL();
    return true;
}

/* Called only by the UWB task: never access SPI from the Bluetooth task. */
static void read_config(void)
{
    current.tx_power_readback = dwt_read_reg(TX_POWER_ID);
    current.chan_readback = dwt_read_reg(CHAN_CTRL_ID);
    current.rf_switch_readback = dwt_read_reg(RF_SWITCH_CTRL_ID);
    uint32_t tx_fctrl = dwt_read_reg(TX_FCTRL_ID);
    uint32_t tx_fctrl_hi = dwt_read_reg(TX_FCTRL_HI_ID);
    uint32_t dtune = dwt_read_reg(DTUNE0_ID);
    current.preamble_readback = (uint16_t)((((tx_fctrl_hi & TX_FCTRL_HI_FINE_PLEN_BIT_MASK) >>
        TX_FCTRL_HI_FINE_PLEN_BIT_OFFSET) + 1U) * 8U);
    current.bitrate_readback = (uint8_t)((tx_fctrl & TX_FCTRL_TXBR_BIT_MASK) >> TX_FCTRL_TXBR_BIT_OFFSET);
    current.pac_readback = (uint8_t)(dtune & DTUNE0_PRE_PAC_SYM_BIT_MASK);
    current.sfd_timeout_readback = (uint16_t)(dtune >> DTUNE0_RX_SFD_TOC_BIT_OFFSET);
    const uint32_t chan_mask = CHAN_CTRL_RX_PCODE_BIT_MASK |
        CHAN_CTRL_TX_PCODE_BIT_MASK | CHAN_CTRL_SFD_TYPE_BIT_MASK | CHAN_CTRL_RF_CHAN_BIT_MASK;
    const uint32_t expected_chan = ((uint32_t)config.rxCode << CHAN_CTRL_RX_PCODE_BIT_OFFSET) |
        ((uint32_t)config.txCode << CHAN_CTRL_TX_PCODE_BIT_OFFSET) |
        ((uint32_t)config.sfdType << CHAN_CTRL_SFD_TYPE_BIT_OFFSET) | (config.chan == 9);
    const uint32_t rf_mask = RF_SWITCH_CTRL_ANTSWEN_BIT_MASK | RF_SWITCH_CTRL_ANTSWCTRL_BIT_MASK;
    const uint32_t expected_rf = RF_SWITCH_CTRL_ANTSWEN_BIT_MASK |
        ((uint32_t)rf_port << RF_SWITCH_CTRL_ANTSWCTRL_BIT_OFFSET);
    current.config_verified = current.tx_power_readback == tx_rf.power &&
        (current.chan_readback & chan_mask) == expected_chan &&
        (current.rf_switch_readback & rf_mask) == expected_rf &&
        current.preamble_readback == config.txPreambLength &&
        current.bitrate_readback == config.dataRate && current.pac_readback == config.rxPAC &&
        current.sfd_timeout_readback == config.sfdTO;
}

static int initialise(void)
{
    current.init_result = 2; /* probing hardware */
    publish();
    int probe_result = UWB_DriverProbe();
    if (probe_result != DWT_SUCCESS) return probe_result;
    current.device_id = dwt_readdevid();
    current.init_result = 3; /* reset / wait for idle */
    publish();
    dwt_softreset(0);
    deca_sleep(2);
    uint32_t start = HAL_GetTick();
    while (!dwt_checkidlerc()) {
        if ((uint32_t)(HAL_GetTick() - start) > 100U) return -2;
        osDelay(1);
    }
    current.init_result = 4; /* driver initialization */
    publish();
    if (dwt_initialise(DWT_DW_INIT) != DWT_SUCCESS) return -3;
    current.init_result = 5; /* radio configuration */
    publish();
    if (dwt_configure(&config) != DWT_SUCCESS) return -4;
    dwt_configuretxrf(&tx_rf);
    /* Single fitted antenna: keep TX and RX on the selected port. */
    dwt_configure_rf_port(rf_port);
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
    /* Initial values only: calibrate antenna delays on both boards. */
    dwt_setrxantennadelay(16385);
    dwt_settxantennadelay(16385);
    UWB_DriverFast();
    read_config();
    dwt_setrxaftertxdelay(0);
    dwt_setrxtimeout(UWB_RX_TIMEOUT_UUS);
    return 0;
}

static void range_once(void)
{
    uint8_t poll[] = {0x41, 0x88, 0, 0x9a, 0x60, 'W', 'A', 'V', 'E', 0xe0, 0, 0};
    const uint32_t events = SYS_STATUS_RXFCG_BIT_MASK |
        SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO;
    current.valid = false;
    poll[2] = sequence++;
    dwt_forcetrxoff();
    dwt_writesysstatuslo(events | SYS_STATUS_TXFRS_BIT_MASK);
    dwt_writetxdata(sizeof poll, poll, 0);
    dwt_writetxfctrl(sizeof poll, 0, 1);
    current.polls++;
    if (dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED) != DWT_SUCCESS) {
        current.errors++;
        return;
    }
    uint32_t start = HAL_GetTick(), status;
    bool tx_seen = false;
    for (;;) {
        status = dwt_readsysstatuslo();
        if (!tx_seen && (status & SYS_STATUS_TXFRS_BIT_MASK)) {
            current.transmitted++;
            tx_seen = true;
        }
        if (status & events) break;
        if ((uint32_t)(HAL_GetTick() - start) >= UWB_EXCHANGE_TIMEOUT_MS) {
            current.timeouts++;
            dwt_forcetrxoff();
            return;
        }
        osDelay(1);
    }
    dwt_writesysstatuslo(events | SYS_STATUS_TXFRS_BIT_MASK);
    if (status & (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO)) {
        if (status & SYS_STATUS_ALL_RX_TO) current.timeouts++;
        else current.errors++;
        return;
    }
    current.frames++;
    dwt_cirdiags_t diagnostics;
    current.rssi_valid =
        dwt_readdiagnostics_acc(&diagnostics, DWT_ACC_IDX_IP_M) == DWT_SUCCESS &&
        dwt_calculate_rssi(&diagnostics, DWT_ACC_IDX_IP_M, &current.rssi_q8) == DWT_SUCCESS;
    if (current.rssi_valid) {
        current.cir_power = diagnostics.power;
        current.accumulation_count = diagnostics.accumCount;
        current.dgc_decision = dwt_get_dgcdecision();
        /* The driver returns SUCCESS even for its invalid-value sentinel. */
        current.rssi_valid = diagnostics.power != 0U && diagnostics.accumCount != 0U &&
            current.rssi_q8 != INT16_MIN;
    }
    uint8_t reply[20];
    if ((dwt_read_reg(RX_FINFO_ID) & 0x3ffU) != sizeof reply) {
        current.rejected++;
        return;
    }
    dwt_readrxdata(reply, sizeof reply, 0);
    uint32_t tx = dwt_readtxtimestamplo32();
    uint32_t rx = dwt_readrxtimestamplo32(IPATOV);
    if (!UWB_DecodeRange(reply, sizeof reply, poll[2], tx, rx,
                         dwt_readclockoffset(), &current.distance_mm)) {
        current.rejected++;
        return;
    }
    current.last_update_ms = HAL_GetTick();
    current.received++;
    current.valid = true;
}

void UWB_RangeTask(void *argument)
{
    (void)argument;
    /* Let Bluetooth/GPS start before touching the optional UWB hardware. */
    osDelay(2000);
    current.init_result = initialise();
    publish();
    if (current.init_result != 0) {
        /* Keep the failure visible, and leave all other application tasks running. */
        for (;;) osDelay(1000);
    }
    uint32_t last_config_ms = HAL_GetTick();
    for (;;) {
        range_once();
        if ((uint32_t)(HAL_GetTick() - last_config_ms) >= 1000U) {
            read_config();
            last_config_ms = HAL_GetTick();
        }
        publish();
        osDelay(100);
    }
}
