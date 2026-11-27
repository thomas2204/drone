"""Exercise the actual ranging task with radio faults, plus phase geometry."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Pdoa(unittest.TestCase):
    def test_measurements_and_geometry(self):
        compiler = Path('C:/MinGW/bin/gcc.exe')
        compiler = str(compiler) if compiler.exists() else shutil.which('gcc')
        self.assertIsNotNone(compiler, 'Native GCC is required')
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'main.h').write_text('#include <stdint.h>\nuint32_t HAL_GetTick(void);\n')
            (work / 'cmsis_os2.h').write_text('#include <stdint.h>\nvoid osDelay(uint32_t);\n')
            (work / 'FreeRTOS.h').write_text(
                '#define taskENTER_CRITICAL() ((void)0)\n#define taskEXIT_CRITICAL() ((void)0)\n')
            (work / 'task.h').write_text('')
            (work / 'app_log.h').write_text(
                '#include <stdint.h>\nint AppLog_Write(const void *, uint16_t, uint32_t);\n')
            (work / 'app_storage.h').write_text('''
#include <stdint.h>
typedef char TCHAR;
typedef uint32_t DWORD;
typedef unsigned UINT;
typedef struct { DWORD cursor; } FIL;
enum { FR_OK, FR_NO_FILE };
enum { FA_READ = 1, FA_WRITE = 2, FA_OPEN_ALWAYS = 16 };
int AppStorage_Open(FIL *, const TCHAR *, uint8_t);
int AppStorage_Close(FIL *);
DWORD f_size(const FIL *);
int f_lseek(FIL *, DWORD);
int f_read(FIL *, void *, UINT, UINT *);
int f_write(FIL *, const void *, UINT, UINT *);
''')
            source = (ROOT / 'Core/Src/uwb_range.c').as_posix()
            (work / 'exercise.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "SOURCE"
static uint32_t now = 10, radio_status;
static int quality_result, sts_bad, frame_length = 20, decode_ok = 1;
static int64_t fake_tdoa;
static uint8_t card_bytes[64];
static DWORD card_size;
int AppLog_Write(const void *p, uint16_t size, uint32_t timeout) {
    (void)p; (void)size; (void)timeout; return 0;
}
int AppStorage_Open(FIL *file, const TCHAR *path, uint8_t mode) {
    assert(strcmp(path, "0:/UWBZERO.BIN") == 0);
    file->cursor = 0;
    return mode == FA_READ && card_size == 0 ? FR_NO_FILE : FR_OK;
}
int AppStorage_Close(FIL *file) { (void)file; return FR_OK; }
DWORD f_size(const FIL *file) { (void)file; return card_size; }
int f_lseek(FIL *file, DWORD offset) { file->cursor = offset; return FR_OK; }
int f_read(FIL *file, void *data, UINT size, UINT *read) {
    assert(file->cursor + size <= card_size);
    memcpy(data, &card_bytes[file->cursor], size);
    file->cursor += size;
    *read = size;
    return FR_OK;
}
int f_write(FIL *file, const void *data, UINT size, UINT *written) {
    assert(file->cursor + size <= sizeof card_bytes);
    memcpy(&card_bytes[file->cursor], data, size);
    file->cursor += size;
    if (file->cursor > card_size) card_size = file->cursor;
    *written = size;
    return FR_OK;
}
uint32_t HAL_GetTick(void) { return now; }
void osDelay(uint32_t ms) { now += ms; }
int UWB_DriverProbe(void) { return 0; }
void UWB_DriverFast(void) {}
uint32_t dwt_readdevid(void) { return DWT_DW3000_PDOA_DEV_ID; }
void dwt_softreset(int32_t n) { (void)n; }
void deca_sleep(unsigned int n) { now += n; }
uint8_t dwt_checkidlerc(void) { return 1; }
int32_t dwt_initialise(int32_t mode) { (void)mode; return 0; }
int32_t dwt_configure(dwt_config_t *c) { (void)c; return 0; }
void dwt_configuretxrf(dwt_txconfig_t *c) { (void)c; }
void dwt_configure_rf_port(dwt_rf_port_ctrl_e p) { (void)p; }
void dwt_setpdoaoffset(uint16_t o) { assert(o == 0); }
void dwt_configciadiag(uint8_t m) { (void)m; }
void dwt_setrxantennadelay(uint16_t n) { (void)n; }
void dwt_settxantennadelay(uint16_t n) { (void)n; }
void dwt_setrxaftertxdelay(uint32_t n) { (void)n; }
void dwt_setrxtimeout(uint32_t n) { (void)n; }
void dwt_forcetrxoff(void) {}
void dwt_writesysstatuslo(uint32_t mask) {
    assert(mask & SYS_STATUS_CIADONE_BIT_MASK); /* No stale CIA completion. */
}
int32_t dwt_writetxdata(uint16_t n, uint8_t *p, uint16_t off) {
    (void)p; assert(n == 12 && off == 0); return 0;
}
void dwt_writetxfctrl(uint16_t n, uint16_t off, uint8_t ranging) {
    assert(n == 12 && off == 0 && ranging == 1);
}
int32_t dwt_starttx(uint8_t mode) {
    assert(mode == (DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED)); return 0;
}
uint32_t dwt_readsysstatuslo(void) { return radio_status; }
uint32_t dwt_read_reg(uint32_t address) {
    assert(address == RX_FINFO_ID); return frame_length;
}
int dwt_readdiagnostics_acc(dwt_cirdiags_t *d, dwt_acc_idx_e i) {
    (void)d; (void)i; return DWT_ERROR;
}
int dwt_calculate_rssi(const dwt_cirdiags_t *d, dwt_acc_idx_e i, int16_t *r) {
    (void)d; (void)i; (void)r; return DWT_ERROR;
}
uint8_t dwt_get_dgcdecision(void) { return 0; }
void dwt_readrxdata(uint8_t *b, uint16_t n, uint16_t off) {
    assert(n == 20 && off == 0); memset(b, 0, n);
}
uint32_t dwt_readtxtimestamplo32(void) { return 0; }
void dwt_readrxtimestamp_ipatov(uint8_t *b) { memset(b, 0, 5); }
int16_t dwt_readclockoffset(void) { return 0; }
bool UWB_DecodeRange(const uint8_t *b, size_t n, uint8_t s,
    uint32_t tx, uint32_t rx, int16_t off, int32_t *d) {
    (void)b;(void)n;(void)s;(void)tx;(void)rx;(void)off;
    *d = 2000; return decode_ok;
}
int16_t dwt_readpdoa(void) { return 1024; }
int32_t dwt_readstsquality(int16_t *q, int32_t seg) {
    assert(seg == 0); *q = quality_result >= 0 ? 100 : 0; return quality_result;
}
int32_t dwt_readstsstatus(uint16_t *s, int32_t seg) {
    *s = sts_bad == seg + 1; return *s ? -1 : 0;
}
void dwt_readtdoa(uint8_t *b) {
    uint64_t bits = (uint64_t)fake_tdoa & ((UINT64_C(1) << 41) - 1);
    for (unsigned i=0; i<6; ++i) b[i] = (uint8_t)(bits >> (i*8));
}
static void good(void) {
    radio_status = SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_CIADONE_BIT_MASK |
        SYS_STATUS_TXFRS_BIT_MASK;
    quality_result=0; sts_bad=0; fake_tdoa=0; decode_ok=1; frame_length=20;
    range_once(); assert(current.valid && current.pdoa_valid);
    assert(!current.angle_valid); /* No invented calibration. */
}
int main(void) {
    int32_t angle = 123;
    float spacing = (299792458.0f / 6489600000.0f) * 0.5f;
    assert(UWB_PhaseToBearing(0, spacing, 0, 5, &angle) && angle == 0);
    assert(UWB_PhaseToBearing(3217, spacing, 0, 5, &angle));
    assert(abs(angle - 30000) < 20);
    assert(UWB_PhaseToBearing(-3217, spacing, 0, 5, &angle));
    assert(abs(angle + 30000) < 20);
    assert(UWB_PhaseToBearing(1024, spacing, 0.5f, 5, &angle) && angle == 0);
    assert(UWB_PhaseToBearing(0, spacing, 6.283185307f, 5, &angle) && abs(angle) < 1);
    assert(!UWB_PhaseToBearing(5000, 0.01f, 0, 5, &angle));
    assert(!UWB_PhaseToBearing(0, 0, 0, 5, &angle));
    assert(!UWB_PhaseToBearing(0, 0.03f, 0, 5, &angle));
    assert(!UWB_PhaseToBearing(0, NAN, 0, 5, &angle));
    assert(!UWB_PhaseToBearing(0, spacing, NAN, 5, &angle));
    assert(!UWB_PhaseToBearing(0, spacing, 0, 6, &angle));
    assert(!UWB_PhaseToBearing(0, spacing, 0, 5, NULL));
    assert(UWB_PhaseToBearing(0, 0.018f, 0, 9, &angle));
    const int64_t tdoa_cases[] = {0,100,-100,101,-101,65536,-65536,
        (INT64_C(1)<<40)-1, -(INT64_C(1)<<40)};
    for (unsigned i=0;i<sizeof(tdoa_cases)/sizeof(tdoa_cases[0]);++i) {
        uint8_t bytes[6]; fake_tdoa=tdoa_cases[i]; dwt_readtdoa(bytes);
        bytes[5] |= 0xfe; assert(UWB_DecodeTdoa(bytes)==fake_tdoa);
    }
    UWB_RangeInit(); good(); assert(current.distance_mm == 2000);
    quality_result=-1; range_once(); assert(current.valid && !current.pdoa_valid);
    good(); sts_bad=1; range_once(); assert(current.valid && !current.pdoa_valid);
    good(); sts_bad=2; range_once(); assert(current.valid && !current.pdoa_valid);
    good(); fake_tdoa=100; range_once(); assert(current.pdoa_valid);
    fake_tdoa=-100; range_once(); assert(current.pdoa_valid);
    fake_tdoa=101; range_once(); assert(!current.pdoa_valid);
    fake_tdoa=-101; range_once(); assert(!current.pdoa_valid);
    fake_tdoa=65536; range_once(); assert(!current.pdoa_valid);
    good(); radio_status &= ~SYS_STATUS_CIADONE_BIT_MASK;
    range_once(); assert(!current.pdoa_valid);
    good(); decode_ok=0; range_once(); assert(!current.valid && !current.pdoa_valid);
    good(); frame_length=19; range_once(); assert(!current.valid && !current.pdoa_valid);
    good(); radio_status=SYS_STATUS_RXFTO_BIT_MASK;
    range_once(); assert(!current.valid && !current.pdoa_valid);
    good(); radio_status=0;
    range_once(); assert(!current.valid && !current.pdoa_valid);
    assert(current.pdoa_rejected >= 7);
    assert(config.pdoaMode == DWT_PDOA_M3 && config.stsLength == DWT_STS_LEN_128);
    assert(config.stsMode == (DWT_STS_MODE_1 | DWT_STS_MODE_SDC));
    assert(rf_port == DWT_RF_PORT_AUTO_1_2);
    UWB_PhaseFilter filter = {0}; int16_t filtered;
    for (unsigned i=0;i<9;++i)
        assert(!UWB_FilterPhase(&filter, 1000, i*100, &filtered));
    assert(UWB_FilterPhase(&filter, 1000, 900, &filtered) && filtered==1000);
    assert(UWB_FilterPhase(&filter, -5000, 1000, &filtered) && filtered==1000);
    assert(UWB_FilterPhase(&filter, -5000, 1100, &filtered) && filtered==1000);
    for(unsigned i=0;i<10;++i) UWB_FilterPhase(&filter,-5000,1200+i*100,&filtered);
    assert(filtered==-5000); /* A real turn eventually replaces the window. */
    assert(!UWB_FilterPhase(&filter, 0, 2700, &filtered)); /* Gap resets history. */
    memset(&filter,0,sizeof(filter));
    const int16_t wrapped[] = {6400,-6400,6420,-6420,6400,-6400,6420,-6420,0,0};
    for(unsigned i=0;i<10;++i) UWB_FilterPhase(&filter,wrapped[i],i*100,&filtered);
    assert(abs(filtered)>6300); /* +/-180 wrap is not a false zero. */
    memset(&filter,0,sizeof(filter));
    for(unsigned i=0;i<10;++i)
        UWB_FilterPhase(&filter,1000,UINT32_MAX-250U+i*100U,&filtered);
    assert(filter.count==10); /* Tick wrap is not a long gap. */
    UWB_DistanceFilter distance={0};int32_t average;
    const int32_t distances[]={100,1000000,3990,4000,4010,3980,4020,4005,3995,4000};
    for(unsigned i=0;i<9;++i) assert(!UWB_FilterDistance(&distance,distances[i],i*100,&average));
    assert(UWB_FilterDistance(&distance,distances[9],900,&average) && average==4000);
    assert(!UWB_FilterDistance(&distance,4000,1501,&average));
    int16_t saved_offset;
    assert(!load_saved_zero(&saved_offset));
    UWB_RangeInit(); assert(!UWB_RangeZeroBearing());
    for(unsigned i=0;i<10;++i) { now+=100; good(); }
    current.init_result=0; publish(); assert(UWB_RangeZeroBearing());
    now+=100; range_once(); assert(current.angle_valid && current.bearing_mdeg==0);
    assert(load_saved_zero(&saved_offset) && saved_offset == 1024);
    card_bytes[card_size++] = 'U'; /* Interrupted write. */
    assert(load_saved_zero(&saved_offset) && saved_offset == 1024);
    assert(save_zero(-2048));
    assert(load_saved_zero(&saved_offset) && saved_offset == -2048);
    card_bytes[card_size - 1U] ^= 1U;
    assert(load_saved_zero(&saved_offset) && saved_offset == 1024);
    publish(); now+=600; assert(!UWB_RangeZeroBearing());
    range_once(); assert(!current.pdoa_filtered_valid && !current.angle_valid);
    puts("PDoA quality, stale/error handling, signed TDoA and bearing geometry passed");
    return 0;
}
'''.replace('SOURCE', source).replace('#include <stdio.h>', '#include <stdio.h>\n#include <stdlib.h>'))
            executable = work / 'pdoa-test.exe'
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I'+str(work), '-I'+str(ROOT/'Core/Inc'),
                            '-I'+str(ROOT/'Drivers/UWB'), '-I'+str(ROOT/'Drivers/UWB/dw3000'),
                            str(work/'exercise.c'), '-lm', '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
