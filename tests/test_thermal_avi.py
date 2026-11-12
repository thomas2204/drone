"""Exercise the imported renderer and streamed AVI writer against a host file."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ThermalAVI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        (cls.work / "ff.h").write_text('''
#pragma once
#include <stdint.h>
#include <stdio.h>
typedef unsigned UINT;
typedef int FRESULT;
typedef struct { FILE *fp; uint32_t pos; } FIL;
#define FR_OK 0
#define FA_CREATE_NEW 4
#define FA_WRITE 2
#define f_tell(f) ((f)->pos)
int f_write(FIL *, const void *, UINT, UINT *);
int f_sync(FIL *);
int f_lseek(FIL *, uint32_t);
int f_truncate(FIL *);
''')
        (cls.work / "main.h").write_text('#include <stdint.h>\nuint32_t HAL_GetTick(void);\n')
        (cls.work / "app_storage.h").write_text('''
#include "ff.h"
int AppStorage_Open(FIL *, const char *, unsigned);
int AppStorage_Close(FIL *);
''')
        (cls.work / "shim.c").write_text('''
#include "ff.h"
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define ftruncate _chsize
#endif
static int writes, fail_at, bad_alignment, opened;
void inject(int n) { fail_at=n; writes=0; }
int alignment_errors(void) { return bad_alignment; }
int files_open(void) { return opened; }
uint32_t HAL_GetTick(void) { static uint32_t t; return t+=250; }
int AppStorage_Open(FIL *f, const char *s, unsigned mode) {
    if (mode!=6) return 1;
    FILE *old=fopen(s,"rb"); if(old) {fclose(old);return 1;}
    f->fp=fopen(s,"w+b"); f->pos=0; if(f->fp) opened++;
    return f->fp?0:1;
}
int AppStorage_Close(FIL *f) { int r=fclose(f->fp); opened--;return r; }
int f_write(FIL *f,const void *p,UINT n,UINT *out) {
    *out=0; writes++;
    unsigned partial=f->pos&511;
    if (((uintptr_t)p&31) || (partial && n>512-partial) || n>1024) {
        bad_alignment++;return 1;
    }
    if(writes==fail_at) {fail_at=0;return 1;}
    *out=fwrite(p,1,n,f->fp);f->pos+=*out;return *out==n?0:1;
}
int f_sync(FIL *f) { return fflush(f->fp); }
int f_lseek(FIL *f,uint32_t p) {
    int r=fseek(f->fp,p,SEEK_SET);if(!r)f->pos=p;return r;
}
int f_truncate(FIL *f) {fflush(f->fp);return ftruncate(fileno(f->fp),f->pos);}
''')
        (cls.work / "exercise.c").write_text(''' 
#include "thermal_avi.h"
void inject(int); int files_open(void); int alignment_errors(void);
int main(void) {
    float frame[768];
    for(int y=0;y<24;y++) for(int x=0;x<32;x++) frame[y*32+x]=20.0f+y;
    if(avi_open("thermal.avi") || avi_add_frame(frame) || avi_add_frame(frame) || avi_close_duration(10000)) return 1;
    if(avi_open("thermal.avi")==0) return 2;
    if(avi_open("failure.avi") || avi_add_frame(frame)) return 3;
    inject(5);
    if(avi_add_frame(frame)==0 || avi_close()) return 4;
    return files_open() || alignment_errors();
}
''')
        out = cls.work / "thermal-test.exe"
        compiler = "C:/MinGW/bin/gcc.exe" if Path("C:/MinGW/bin/gcc.exe").exists() else shutil.which("gcc")
        subprocess.run([compiler, "-O2", "-std=gnu11",
                        "-I" + str(cls.work), "-I" + str(ROOT / "Core/Inc"),
                        str(ROOT / "Core/Src/thermal_avi.c"),
                        str(ROOT / "Core/Src/thermal_palette.c"),
                        str(cls.work / "shim.c"), str(cls.work / "exercise.c"), "-lm", "-o", str(out)], check=True)
        subprocess.run([str(out)], cwd=cls.work, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_render_container_and_alignment(self):
        name = self.work / "thermal.avi"
        data = name.read_bytes()
        u32 = lambda n: struct.unpack_from('<I', data, n)[0]
        self.assertEqual(data[:4], b'RIFF')
        self.assertEqual(u32(4), len(data)-8)
        self.assertEqual(data[8:12], b'AVI ')
        self.assertEqual(u32(48), 2)
        self.assertEqual(u32(32), 5000000)  # Two frames span exactly 10 seconds.
        self.assertEqual(u32(132), 200)  # dwRate / dwScale = 0.2 fps.
        self.assertEqual((u32(176), u32(180)), (320, 240))
        self.assertEqual(data[224:228], b'00db')
        self.assertEqual(u32(228), 230400)
        # Bottom-up storage: the warm last sensor row comes first.
        self.assertGreater(sum(data[232:235]), sum(data[232+239*960:235+239*960]))
        idx = 224 + 2*(230400+8)
        self.assertEqual(data[idx:idx+4], b'idx1')
        self.assertEqual(u32(idx+4), 32)
        self.assertEqual(u32(idx+16), 4)
        self.assertEqual(u32(idx+32), 4+230408)

    def test_failed_frame_is_truncated_before_index(self):
        name = self.work / "failure.avi"
        data = name.read_bytes()
        self.assertEqual(struct.unpack_from('<I', data, 48)[0], 1)
        self.assertEqual(len(data), 224+230408+8+16)
        self.assertEqual(data[-24:-20], b'idx1')


if __name__ == '__main__':
    unittest.main()
