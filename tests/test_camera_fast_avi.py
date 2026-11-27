"""Validate VGA/UXGA JPEG parsing and bounded MJPEG AVI output."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


class FastCameraAVI(unittest.TestCase):
    def test_vga_jpeg_150_frames_ten_seconds_and_capacity(self):
        self.exercise_recording(640, 480)

    def test_uxga_jpeg_above_old_capture_limit(self):
        self.exercise_recording(1600, 1200)

    def test_svga_recording(self):
        self.exercise_recording(800, 600)

    def test_xga_recording(self):
        self.exercise_recording(1024, 768)

    def test_1280_recording(self):
        self.exercise_recording(1280, 960)

    def exercise_recording(self, width, height):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            image = Image.new('RGB', (width, height))
            image.putdata([(x % 256, y % 256, (x+y) % 256)
                           for y in range(height) for x in range(width)])
            image.save(work / 'vga.jpg', quality=95 if width == 1600 else 80, subsampling=2)
            if width == 1600:
                self.assertGreater((work/'vga.jpg').stat().st_size, 128*1024)
                self.assertLessEqual((work/'vga.jpg').stat().st_size, 240*1024)
            (work / 'exercise.c').write_text('''
#include "camera_avi.h"
#include "camera_jpeg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool write_file(void *f,const void *p,uint32_t n) {return fwrite(p,1,n,f)==n;}
static bool seek_file(void *f,uint32_t n) {return fseek(f,n,SEEK_SET)==0;}
int main(void) {
    FILE *input=fopen("vga.jpg","rb");assert(input);
    fseek(input,0,SEEK_END);long bytes=ftell(input);rewind(input);
    unsigned char *jpeg=malloc(bytes);assert(jpeg);
    assert(fread(jpeg,1,bytes,input)==(size_t)bytes);fclose(input);
    size_t offset,length;uint16_t width,height;
    assert(CameraJPEG_Find(jpeg,bytes,&offset,&length,&width,&height));
    assert(width==EXPECT_WIDTH && height==EXPECT_HEIGHT && offset==0 && length==(size_t)bytes);
    /* Incremental DMA polling, including EOI split between polls. */
    size_t checked=0;
    for(size_t n=1;n<(size_t)bytes;n+=17)
        assert(!CameraJPEG_FindIncremental(jpeg,n,&checked,&offset,&length,&width,&height));
    assert(!CameraJPEG_FindIncremental(jpeg,bytes-1,&checked,&offset,&length,&width,&height));
    assert(CameraJPEG_FindIncremental(jpeg,bytes,&checked,&offset,&length,&width,&height));
    assert(offset==0 && length==(size_t)bytes && width==EXPECT_WIDTH && height==EXPECT_HEIGHT);
    /* FFD9 inside an APP payload is not an actual end of image. */
    unsigned char *metadata=malloc(bytes+8);assert(metadata);
    const unsigned char prefix[]={0xff,0xd8,0xff,0xe1,0,6,0,0xff,0xd9,0};
    memcpy(metadata,prefix,sizeof(prefix));memcpy(metadata+10,jpeg+2,bytes-2);
    checked=0;
    assert(!CameraJPEG_FindIncremental(metadata,10,&checked,&offset,&length,&width,&height));
    assert(CameraJPEG_FindIncremental(metadata,bytes+8,&checked,&offset,&length,&width,&height));
    assert(offset==0 && length==(size_t)bytes+8);free(metadata);
    /* Follow a damaged frame with an intact frame in one DMA buffer. */
    size_t damaged = (size_t)bytes - 2;
    unsigned char *stream = malloc(damaged + bytes);assert(stream);
    memcpy(stream,jpeg,damaged);
    memcpy(stream+damaged,jpeg,bytes);
    assert(!CameraJPEG_Find(stream,damaged,&offset,&length,&width,&height));
    assert(CameraJPEG_Find(stream,damaged+bytes,&offset,&length,&width,&height));
    assert(width==EXPECT_WIDTH && height==EXPECT_HEIGHT && offset==damaged && length==(size_t)bytes);
    checked=0;
    assert(!CameraJPEG_FindIncremental(stream,damaged,&checked,&offset,&length,&width,&height));
    assert(CameraJPEG_FindIncremental(stream,damaged+bytes,&checked,&offset,&length,&width,&height));
    assert(offset==damaged && length==(size_t)bytes);
    free(stream);
    CameraAVI avi;FILE *out=fopen("vga.avi","w+b");assert(out);
    assert(CameraAVI_Begin(&avi,write_file,seek_file,out,width,height));
    assert(!CameraAVI_Frame(&avi,jpeg,CAMERA_AVI_MAX_JPEG_BYTES+1U));
    for(unsigned i=0;i<150;i++) assert(CameraAVI_Frame(&avi,jpeg,bytes));
    assert(CameraAVI_End(&avi,10000));assert(fclose(out)==0);
    out=fopen("capacity.avi","w+b");assert(out);
    assert(CameraAVI_Begin(&avi,write_file,seek_file,out,width,height));
    for(unsigned i=0;i<CAMERA_AVI_MAX_FRAMES;i++) assert(CameraAVI_Frame(&avi,jpeg,bytes));
    assert(!CameraAVI_Frame(&avi,jpeg,bytes));
    assert(CameraAVI_End(&avi,10000));fclose(out);free(jpeg);return 0;
}
'''.replace('EXPECT_WIDTH', str(width)).replace('EXPECT_HEIGHT', str(height)))
            compiler = 'C:/MinGW/bin/gcc.exe' if Path('C:/MinGW/bin/gcc.exe').exists() else shutil.which('gcc')
            exe = work / 'test.exe'
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT/'Core/Inc'), str(work/'exercise.c'),
                            str(ROOT/'Core/Src/camera_avi.c'),
                            str(ROOT/'Core/Src/camera_jpeg.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], cwd=work, check=True)
            data=(work/'vga.avi').read_bytes()
            u32=lambda at: struct.unpack_from('<I',data,at)[0]
            self.assertEqual(u32(4)+8,len(data))
            self.assertEqual((u32(176),u32(180)),(width,height))
            self.assertEqual(data[112:116],b'MJPG')
            self.assertEqual((u32(48),u32(140)),(150,150))
            self.assertEqual(u32(140)*u32(128)/u32(132),10)
            self.assertEqual(u32(132)/u32(128),15)
            at=224
            for _ in range(150):
                self.assertEqual(data[at:at+4],b'00dc')
                size=u32(at+4)
                self.assertEqual(data[at+8:at+8+size],(work/'vga.jpg').read_bytes())
                at+=8+size+(size&1)
            self.assertEqual(data[at:at+4],b'idx1')
            self.assertEqual(u32(at+4),150*16)
            index=at+8
            self.assertEqual(data[index:index+4],b'00dc')
            self.assertEqual(u32(index+8),4)
            self.assertEqual(u32(index+149*16+8),4+149*(8+size+(size&1)))
            ffmpeg=list((ROOT/'build/video-tools').glob('**/ffmpeg*.exe'))
            if ffmpeg:
                subprocess.run([str(ffmpeg[0]),'-hide_banner','-loglevel','error','-xerror',
                                '-i',str(work/'vga.avi'),'-f','null','-'],check=True,timeout=30)


if __name__ == '__main__':
    unittest.main()
