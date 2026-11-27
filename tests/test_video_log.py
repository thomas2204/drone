"""Exercise real telemetry snapshot selection, video time mapping and CSV I/O."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class VideoLog(unittest.TestCase):
    def test_time_fix_loss_staleness_calibration_and_storage_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'ff.h').write_text('''
#pragma once
#include <stdint.h>
typedef unsigned UINT;
typedef struct { int unused; } FIL;
typedef enum {FR_OK,FR_DISK_ERR,FR_INT_ERR,FR_NO_FILE,FR_EXIST,FR_INVALID_PARAMETER} FRESULT;
#define FA_CREATE_NEW 4
#define FA_WRITE 2
#define FA_READ 1
FRESULT f_write(FIL *,const void *,UINT,UINT *);
FRESULT f_sync(FIL *);
FRESULT f_read(FIL *,void *,UINT,UINT *);
uint32_t f_size(FIL *);
''')
            (work / 'app_storage.h').write_text('''
#include "ff.h"
FRESULT AppStorage_Open(FIL *,const char *,unsigned);
FRESULT AppStorage_Close(FIL *);
''')
            (work / 'main.h').write_text('#include <stdint.h>\nuint32_t HAL_GetTick(void);\n')
            source = (ROOT / 'Core/Src/video_log.c').as_posix()
            (work / 'exercise.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "SOURCE"
static uint32_t now;
static UWB_RangeSnapshot fake_uwb;
static GPS_RawSnapshot_t fake_gps;
static bool gps_available, advance_clock;
static char stored[128000];static size_t stored_bytes;
static bool short_write, sync_error, corrupt_read;static unsigned closed;
static size_t read_cursor;
uint32_t HAL_GetTick(void) {return now;}
bool UWB_RangeGetSnapshot(UWB_RangeSnapshot *out) {*out=fake_uwb;return true;}
bool GPS_RawGetSnapshot(GPS_RawSnapshot_t *out) {
    *out=fake_gps;if(advance_clock) out->last_pvt_ms=++now;return gps_available;
}
FRESULT AppStorage_Open(FIL *f,const char *name,unsigned mode) {
    (void)f;assert(!strcmp(name,"0:/VIDEO123.CSV"));
    assert(mode==(FA_CREATE_NEW|FA_WRITE) || mode==FA_READ);
    if(mode&FA_WRITE) stored_bytes=0; else read_cursor=0;
    return FR_OK;
}
FRESULT AppStorage_Close(FIL *f) {(void)f;++closed;return FR_OK;}
FRESULT f_sync(FIL *f) {(void)f;return sync_error?FR_DISK_ERR:FR_OK;}
uint32_t f_size(FIL *f) {(void)f;return (uint32_t)stored_bytes;}
FRESULT f_read(FIL *f,void *data,UINT n,UINT *received) {
    (void)f;assert(read_cursor+n<=stored_bytes);
    memcpy(data,stored+read_cursor,n);read_cursor+=n;*received=n;
    if(corrupt_read && n) ((unsigned char *)data)[0]^=1;
    return FR_OK;
}
FRESULT f_write(FIL *f,const void *data,UINT n,UINT *written) {
    (void)f;assert(stored_bytes+n<sizeof stored);
    memcpy(stored+stored_bytes,data,n);stored_bytes+=n;stored[stored_bytes]=0;
    *written=short_write?n-1:n;return FR_OK;
}
int main(void) {
    VideoLog_Begin();now=1000;VideoLog_Sample(0);
    char line[224];
    assert(format_row(line,sizeof line,&rows[0],0,300,30000,1000)>0);
    assert(strstr(line,",NA,NA,NA,NA,NA,NA,0,0\r\n"));
    now=11000;
    fake_uwb.init_result=0;fake_uwb.valid=true;fake_uwb.last_update_ms=now;
    fake_uwb.distance_filtered_valid=true;fake_uwb.distance_filtered_mm=4000;
    fake_uwb.angle_valid=true;fake_uwb.angle_calibrated=true;
    fake_uwb.bearing_mdeg=12345;fake_uwb.zero_phase_raw=1024;
    fake_uwb.distance_samples=fake_uwb.heading_samples=10;
    gps_available=true;fake_gps.pvt_count=1;fake_gps.last_pvt_ms=now;
    fake_gps.pvt.fix_ok=true;fake_gps.pvt.fix_type=3;
    fake_gps.pvt.latitude_e7=223344556;fake_gps.pvt.longitude_e7=1131234567;
    fake_gps.pvt.altitude_msl_mm=123450;
    VideoLog_Sample(100);
    assert(format_row(line,sizeof line,&rows[100],100,300,30000,1000)>0);
    assert(!strcmp(line,"10.000,10000,100,10000,11000,4.000,12.345,22.3344556,113.1234567,123.450,1024,10,10\r\n"));
    fake_gps.pvt.fix_ok=false;VideoLog_Sample(101);
    assert(!(rows[101].flags&(POSITION|ALTITUDE))); /* No cached good fix. */
    fake_gps.pvt.fix_ok=true;fake_gps.pvt.fix_type=2;VideoLog_Sample(102);
    assert((rows[102].flags&POSITION) && !(rows[102].flags&ALTITUDE));
    now+=1500;VideoLog_Sample(103);
    assert(!(rows[103].flags&(RANGE|HEADING|POSITION|ALTITUDE)));
    assert(rows[103].flags&CALIBRATED); /* Calibration persists, measurement does not. */
    fake_gps.pvt_count=0;fake_gps.gga_count=fake_gps.gsa_decoded_count=1;
    fake_gps.last_gga_ms=fake_gps.last_gsa_ms=now;
    fake_gps.fix.position_valid=fake_gps.fix.altitude_valid=true;
    fake_gps.fix.fix_quality=1;fake_gps.gsa.fix_dimension=3;
    fake_gps.fix.latitude_e7=-123456789;fake_gps.fix.longitude_e7=-345678901;
    fake_gps.fix.altitude_msl_mm=-1250;VideoLog_Sample(104);
    assert((rows[104].flags&(POSITION|ALTITUDE))==(POSITION|ALTITUDE));
    assert(format_row(line,sizeof line,&rows[104],104,300,30000,1000)>0);
    assert(strstr(line,"-12.3456789,-34.5678901,-1.250"));
    fake_gps.pvt_count=1;fake_gps.last_pvt_ms=now-1500;VideoLog_Sample(105);
    assert(!(rows[105].flags&POSITION)); /* No fallback to older NMEA once PVT starts. */
    advance_clock=true;fake_gps.pvt.fix_type=3;VideoLog_Sample(106);
    assert(rows[106].uptime_ms==now && (rows[106].flags&POSITION));
    advance_clock=false;
    VideoRow wrap={.uptime_ms=9};
    assert(format_row(line,sizeof line,&wrap,1,2,200,UINT32_MAX-10)>0);
    assert(strstr(line,"0.100,100,1,20,9,"));
    FIL file;uint8_t scratch[512];
    assert(VideoLog_Write(&file,"0:/VIDEO123.AVI",101,10100,1000,scratch,sizeof scratch)==FR_OK);
    assert(video_log.rows==101 && closed==2 && video_log.bytes==video_log.verified_bytes);
    assert(strstr(stored,"10.000,10000,100,10000,11000,4.000,12.345,22.3344556"));
    assert(!strncmp(stored,header,sizeof header-1));
    short_write=true;assert(VideoLog_Write(&file,"0:/VIDEO123.AVI",1,100,1000,scratch,sizeof scratch)==FR_DISK_ERR);
    short_write=false;sync_error=true;
    assert(VideoLog_Write(&file,"0:/VIDEO123.AVI",1,100,1000,scratch,sizeof scratch)==FR_DISK_ERR);
    assert(closed==4);
    sync_error=false;corrupt_read=true;
    assert(VideoLog_Write(&file,"0:/VIDEO123.AVI",1,100,1000,scratch,sizeof scratch)==FR_INT_ERR);
    assert(closed==6);
    puts("CSV video timeline, NA/fix loss, units, saved zero, tick wrap and SD failures passed");
    return 0;
}
'''.replace('SOURCE', source))
            compiler = 'C:/MinGW/bin/gcc.exe' if Path('C:/MinGW/bin/gcc.exe').exists() else shutil.which('gcc')
            exe = work / 'video-log.exe'
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(work), '-I'+str(ROOT/'Core/Inc'),
                            str(work/'exercise.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
