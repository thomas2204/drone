/* Initiator matching UWB-beacon-CMake-version/Core/Src/beacon_twr.c.
 * Poll -> response carrying two LE32 timestamps; no final packet. */
#include "uwb_range.h"
#include "uwb_protocol.h"
#include "uwb_angle.h"
#include "app_log.h"
#include "app_storage.h"
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
static UWB_PhaseFilter phase_filter;
static bool zero_pending;
static int16_t zero_raw;
static float phase_offset_rad = UWB_PHASE_OFFSET_RAD;
static const TCHAR zero_path[] = "0:/UWBZERO.BIN";
static const uint8_t zero_signature[] = {'U', 'W', 'B', '1'};
#define ZERO_RECORD_SIZE 8U
/* Two antennas on RF1/RF2; hardware switches during the STS sequence. */
static const dwt_rf_port_ctrl_e rf_port = DWT_RF_PORT_AUTO_1_2;
/* Match the 4096-symbol F103 test profile. Its response is scheduled 8000 us
 * after poll RX; leave a 20000-UUS (~20.51 ms) receive window. */
#define UWB_RX_TIMEOUT_UUS 20000U
#define UWB_EXCHANGE_TIMEOUT_MS 40U
static dwt_config_t config = {
    .chan = 5, .txPreambLength = DWT_PLEN_4096, .rxPAC = DWT_PAC32,
    .txCode = 9, .rxCode = 9, .sfdType = 2, /* Qorvo 16-symbol SFD */
    .dataRate = DWT_BR_850K,
    .phrMode = DWT_PHRMODE_STD, .phrRate = DWT_PHRRATE_STD,
    .sfdTO = 4097, /* Extended receiver SFD wait for the outdoor range test. */
    /* Deterministic bench STS: no KEY/IV synchronization, no secure ranging. */
    .stsMode = DWT_STS_MODE_1 | DWT_STS_MODE_SDC,
    .stsLength = DWT_STS_LEN_128,
    .pdoaMode = DWT_PDOA_M3
};
/* Match the peer's requested power; FE is coarse=2/fine=63, not maximum FF. */
static dwt_txconfig_t tx_rf = { .PGdly = 0x34, .power = 0xFEFEFEFEUL, .PGcount = 0 };


static void publish(void)
{
    taskENTER_CRITICAL();
    shared = current;
    taskEXIT_CRITICAL();
}

static bool load_saved_zero(int16_t *offset)
{
    FIL file;
    if (AppStorage_Open(&file, zero_path, FA_READ) != FR_OK) return false;

    bool found = false;
    DWORD end = f_size(&file);
    end -= end % ZERO_RECORD_SIZE;
    /* A partial or corrupt final save must not hide the preceding calibration. */
    while (end >= ZERO_RECORD_SIZE) {
        uint8_t record[ZERO_RECORD_SIZE];
        UINT size;
        end -= ZERO_RECORD_SIZE;
        if (f_lseek(&file, end) != FR_OK ||
            f_read(&file, record, sizeof record, &size) != FR_OK ||
            size != sizeof record)
            break;
        if (memcmp(record, zero_signature, sizeof zero_signature) != 0 ||
            record[4] + record[6] != UINT8_MAX || record[5] + record[7] != UINT8_MAX)
            continue;
        const int32_t value = (int32_t)record[4] | (int32_t)record[5] << 8;
        *offset = (int16_t)(value <= INT16_MAX ? value : value - 65536);
        found = true;
        break;
    }
    return AppStorage_Close(&file) == FR_OK && found;
}

static bool save_zero(int16_t offset)
{
    FIL file;
    if (AppStorage_Open(&file, zero_path, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK)
        return false;

    const uint16_t value = (uint16_t)offset;
    uint8_t record[ZERO_RECORD_SIZE];
    memcpy(record, zero_signature, sizeof zero_signature);
    record[4] = (uint8_t)value;
    record[5] = (uint8_t)(value >> 8);
    record[6] = (uint8_t)~record[4];
    record[7] = (uint8_t)~record[5];
    const DWORD end = f_size(&file);
    UINT size;
    const bool written = f_lseek(&file, end - end % ZERO_RECORD_SIZE) == FR_OK &&
        f_write(&file, record, sizeof record, &size) == FR_OK && size == sizeof record;
    return AppStorage_Close(&file) == FR_OK && written;
}

void UWB_RangeInit(void)
{
    memset(&current, 0, sizeof current);
    memset(&phase_filter, 0, sizeof phase_filter);
    zero_pending = false;
    phase_offset_rad = UWB_PHASE_OFFSET_RAD;
    current.init_result = 1;
    current.rf_port = (uint8_t)rf_port;
    current.angle_calibrated = UWB_ANGLE_CALIBRATED;
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
    /* 4096 is encoded through TXPSR with FINE_PLEN cleared, unlike <=2048. */
    if ((tx_fctrl_hi & TX_FCTRL_HI_FINE_PLEN_BIT_MASK) == 0U &&
        ((tx_fctrl & TX_FCTRL_TXPSR_BIT_MASK) >> TX_FCTRL_TXPSR_BIT_OFFSET) == 3U) {
        current.preamble_readback = 4096U;
    }
    current.bitrate_readback = (uint8_t)((tx_fctrl & TX_FCTRL_TXBR_BIT_MASK) >> TX_FCTRL_TXBR_BIT_OFFSET);
    current.pac_readback = (uint8_t)(dtune & DTUNE0_PRE_PAC_SYM_BIT_MASK);
    current.sfd_timeout_readback = (uint16_t)(dtune >> DTUNE0_RX_SFD_TOC_BIT_OFFSET);
    const uint32_t chan_mask = CHAN_CTRL_RX_PCODE_BIT_MASK |
        CHAN_CTRL_TX_PCODE_BIT_MASK | CHAN_CTRL_SFD_TYPE_BIT_MASK | CHAN_CTRL_RF_CHAN_BIT_MASK;
    const uint32_t expected_chan = ((uint32_t)config.rxCode << CHAN_CTRL_RX_PCODE_BIT_OFFSET) |
        ((uint32_t)config.txCode << CHAN_CTRL_TX_PCODE_BIT_OFFSET) |
        ((uint32_t)config.sfdType << CHAN_CTRL_SFD_TYPE_BIT_OFFSET) | (config.chan == 9);
    const uint32_t rf_mask = RF_SWITCH_CTRL_ANTSWEN_BIT_MASK |
        RF_SWITCH_CTRL_ANTSWCTRL_BIT_MASK | RF_SWITCH_CTRL_ANT_SW_PDOA_PORT_BIT_MASK;
    /* AUTO_1_2 clears manual control and selects RF1 as the primary port. */
    const uint32_t expected_rf = 0U;
    current.sys_cfg_readback = dwt_read_reg(SYS_CFG_ID);
    current.sts_length_readback = (uint8_t)(dwt_read_reg(STS_CFG0_ID) & STS_CFG0_CPS_LEN_BIT_MASK);
    const uint32_t mode_mask = SYS_CFG_PDOA_MODE_BIT_MASK |
        SYS_CFG_CP_SPC_BIT_MASK | SYS_CFG_CP_SDC_BIT_MASK;
    const uint32_t expected_mode = ((uint32_t)config.pdoaMode << SYS_CFG_PDOA_MODE_BIT_OFFSET) |
        ((uint32_t)config.stsMode << SYS_CFG_CP_SPC_BIT_OFFSET);
    current.config_verified = current.tx_power_readback == tx_rf.power &&
        (current.chan_readback & chan_mask) == expected_chan &&
        (current.rf_switch_readback & rf_mask) == expected_rf &&
        (current.sys_cfg_readback & mode_mask) == expected_mode &&
        current.sts_length_readback == config.stsLength &&
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
    if (current.device_id != DWT_DW3000_PDOA_DEV_ID) return -6;
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
    /* Manual RF selection disables PDoA. */
    dwt_configure_rf_port(rf_port);
    /* Apply the measured phase offset once in software, not twice. */
    dwt_setpdoaoffset(0);
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

bool UWB_RangeZeroBearing(void)
{
    taskENTER_CRITICAL();
    bool ready = shared.init_result == 0 && shared.valid &&
        shared.pdoa_valid && shared.pdoa_filtered_valid &&
        (uint32_t)(HAL_GetTick() - shared.last_update_ms) < 500U;
    if (ready) {
        zero_raw = shared.pdoa_filtered_raw;
        zero_pending = true;
    }
    taskEXIT_CRITICAL();
    return ready;
}

static void range_once(void)
{
    taskENTER_CRITICAL();
    const bool have_zero = zero_pending;
    const int16_t offset = zero_raw;
    zero_pending = false;
    taskEXIT_CRITICAL();
    if (have_zero) {
        phase_offset_rad = (float)offset / 2048.0f;
        current.angle_calibrated = true;
        const char *message = save_zero(offset) ?
            "UWB: forward zero saved to SD\r\n" :
            "UWB: forward zero applied; SD save failed (RAM only)\r\n";
        AppLog_Write(message, (uint16_t)strlen(message), 100);
    }
    uint8_t poll[] = {0x41, 0x88, 0, 0x9a, 0x60, 'W', 'A', 'V', 'E', 0xe0, 0, 0};
    const uint32_t events = SYS_STATUS_RXFCG_BIT_MASK |
        SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO;
    current.valid = false;
    current.pdoa_valid = false;
    current.angle_valid = false;
    current.pdoa_filtered_valid = false;
    poll[2] = sequence++;
    dwt_forcetrxoff();
    dwt_writesysstatuslo(events | SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_CIADONE_BIT_MASK);
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
    dwt_writesysstatuslo(events | SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_CIADONE_BIT_MASK);
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
    /* This SDK ignores the segment argument to readrxtimestamplo32(). Use
     * the explicit Ipatov API on BOTH peers to preserve the existing TWR basis. */
    uint8_t rx_ts[5];
    dwt_readrxtimestamp_ipatov(rx_ts);
    uint32_t rx = (uint32_t)rx_ts[0] | (uint32_t)rx_ts[1] << 8 |
        (uint32_t)rx_ts[2] << 16 | (uint32_t)rx_ts[3] << 24;
    current.clock_offset_raw = dwt_readclockoffset();
    if (!UWB_DecodeRange(reply, sizeof reply, poll[2], tx, rx,
                         current.clock_offset_raw, &current.distance_mm)) {
        current.rejected++;
        return;
    }
    current.last_update_ms = HAL_GetTick();
    current.received++;
    current.valid = true;
    /* A good payload CRC alone does not establish a good PDoA measurement. */
    current.pdoa_raw = dwt_readpdoa();
    int32_t quality = dwt_readstsquality(&current.sts_quality, 0);
    int32_t sts0 = dwt_readstsstatus(&current.sts_status[0], 0);
    int32_t sts1 = dwt_readstsstatus(&current.sts_status[1], 1);
    uint8_t tdoa[6];
    dwt_readtdoa(tdoa);
    current.tdoa_raw = UWB_DecodeTdoa(tdoa);
    current.pdoa_valid = quality >= 0 && sts0 == 0 && sts1 == 0 &&
        (status & SYS_STATUS_CIADONE_BIT_MASK) != 0U &&
        (status & (SYS_STATUS_CIAERR_BIT_MASK | SYS_STATUS_CPERR_BIT_MASK)) == 0U &&
        current.tdoa_raw >= -DWT_VALID_TDOA_LIMIT &&
        current.tdoa_raw <= DWT_VALID_TDOA_LIMIT;
    if (!current.pdoa_valid) current.pdoa_rejected++;
    if (current.pdoa_valid)
        current.pdoa_filtered_valid = UWB_FilterPhase(&phase_filter,
            current.pdoa_raw, HAL_GetTick(), &current.pdoa_filtered_raw);
    if (current.pdoa_filtered_valid && current.angle_calibrated)
        current.angle_valid = UWB_PhaseToBearing(current.pdoa_filtered_raw,
            UWB_ANTENNA_SPACING_M, phase_offset_rad, config.chan,
            &current.bearing_mdeg);
}

void UWB_RangeTask(void *argument)
{
    (void)argument;
    /* Let Bluetooth/GPS start before touching the optional UWB hardware. */
    osDelay(2000);
    current.init_result = initialise();
    int16_t saved_offset;
    if (current.init_result == 0 && load_saved_zero(&saved_offset)) {
        phase_offset_rad = (float)saved_offset / 2048.0f;
        current.angle_calibrated = true;
        static const char message[] = "UWB: loaded saved forward zero from SD\r\n";
        AppLog_Write(message, sizeof message - 1U, 100);
    }
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
