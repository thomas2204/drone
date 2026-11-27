# H743 camera recording

## Approximately 10 FPS normal Debug profile (2026-11-27)

Normal SDRAM Debug now defaults to native **800x600 MJPEG at a 100 ms target
period**. JPEG quantization remains 20, so the speed tradeoff is resolution,
not stronger compression. The 240 KiB SDRAM capture buffer is retained.
Flow builds keep their calibrated 640x480 profile; software H.264/RGB24 stay
320x240. The normal recording controls and Debug flash entry are unchanged.

The capture loop scans only newly arrived DMA bytes for JPEG EOI candidates,
then uses the existing full parser to validate the image. Split EOI markers,
EOI-like APP metadata and recovery after a damaged JPEG have regression tests.
SDRAM builds stage SD I/O in aligned 16 KiB chunks rather than 4 KiB; the
internal-SRAM builds keep 4 KiB. No unvalidated cache changes are used.

Connected-board comparisons, with real captures and SD readback verification:

| Resolution | Frames | Duration | Average FPS |
| --- | ---: | ---: | ---: |
| 1600x1200 | 60 | 11914 ms | 5.04 |
| 1280x960 | 60 | 11911 ms | 5.04 |
| 1024x768 | 60 | 11902 ms | 5.04 |
| 800x600 | 100 | 10026 ms | 9.97 |

The 800x600 run verified all 3452174 JPEG payload bytes with no DMA, DCMI,
SD or FatFs errors (VIDEO012.AVI). A DVP divider change from 12 to 8 did not
improve full-resolution FPS. An experimental capture/write overlap also
remained near 5 FPS and is not included in the final implementation. These
tests do not establish that the camera hardware could never reach 10 FPS at
UXGA; they show the limit of the configurations tested on this board.

`camera_snapshot_jpeg_width` accepts 640, 800, 1024, 1280 or 1600 before a new
request, with height at 3/4 of width. The larger modes remain available but
did not meet 10 FPS in these tests. Flow/internal-SRAM builds permit only 640.
800x600 has 56% more pixels than the original 640x480 recording, but 75% fewer
pixels than 1600x1200. The source retains the original DVP timing profiles.

Evidence: `build/camera-10fps-1600-result.txt`,
`build/camera-10fps-1600-div8-result.txt`, `build/camera-10fps-1280-result.txt`,
`build/camera-10fps-1024-result.txt`, `build/camera-10fps-800-result.txt` and
`build/camera-10fps-host-tests.txt`. Earlier sections below describe the
previous defaults and their validation results.

The final default build repeated 100 frames in 10024 ms (9.98 FPS), verifying
3495340 payload bytes. A camera-only session through the normal recording
coordinator then saved **298 frames in 30000 ms (9.93 FPS)** and verified all
10299756 JPEG payload bytes. Coordinator `active=0`, `failed=0`; camera DMA,
DCMI, SD, FatFs, close and unmount results were zero. The coordinator was
started through its debugger-visible request state with the same 30-second
duration as `record cam`; no flight-control command was sent. Simultaneous
thermal recording was not part of this FPS measurement.

A separate four-frame 800x600 clip was exported from SD as
`build/camera-10fps-video.avi` (150412 bytes). Header/index/RIFF checks agree,
and FFmpeg decoded all frames with `-xerror`, exit 0. Final evidence:
`build/camera-10fps-final-100-result.txt`,
`build/camera-10fps-normal-complete-result.txt`,
`build/camera-10fps-export-result.txt`,
`build/camera-10fps-container-result.json` and `build/camera-10fps-decode.log`.
Five host cases cover VGA, SVGA, XGA, 1280x960 and UXGA; normal Debug, SDRAM
flow and internal-SRAM Release builds pass. Left the normal Debug firmware
running with width 800, export/request disabled and frame-count setting 40;
the temporary GDB server was stopped after detach. Flash was backed up before
the first FPS trial to `build/before-10fps-flash.bin`.

## Native 1600x1200 recording with SDRAM (2026-11-27)

The normal Debug preset now defaults to 1600x1200 (UXGA) for OV2640 JPEG
photos and direct MJPEG AVI. The capture buffer is 240 KiB in the tested lower
2 MiB SDRAM region. Native UXGA sensor window/timing is retained from the
[pinned Espressif driver](https://github.com/espressif/esp32-camera/blob/b3f7fe802f41f17fd7a104d517c53f1f62c8414d/sensors/private_include/ov2640_settings.h),
including the output size's high bits and the UXGA JPEG DVP divider of 12.
This captures the native sensor window rather than enlarging a VGA JPEG.

Build and flash the usual **STM32Cube: Drone + MP4 camera (Debug)** entry.
Direct MJPEG remains the default format despite the launch entry's older name.
The unchanged 320x240 software H.264 and RGB24 modes use their existing buffers.
Flow builds retain 640x480 to preserve their established tracking/calibration.

Debugger variable `camera_snapshot_jpeg_width` selects 640, 800 or 1600 before
the next JPEG/MJPEG request (height is 3/4 of width). SDRAM Debug defaults to
1600; internal-SRAM and flow builds permit only 640. Unsupported settings
report CAMERA_RESOLUTION (14). Nominal pacing is 200 ms for UXGA (5 FPS cap),
100 ms for SVGA and 67 ms for VGA; capture and SD delays can reduce actual FPS.
The existing ten-second recording command remains unchanged.

Both capture and the AVI writer permit JPEG payloads up to 240 KiB. A capture
larger than its bounded buffer reports overflow instead of claiming success.
The DMA request is 61440 words, below its 65535-word single-transfer limit;
larger HAL multi-buffer transfers are intentionally not used by the current
NDTR accounting. SD writes continue through the aligned internal SRAM buffer.

Regression coverage exercises JPEG parsing and finalized MJPEG AVI at VGA and
UXGA, including a generated UXGA JPEG above the previous 128 KiB ceiling,
recovery after a damaged JPEG, the frame-count limit and rejection above the
new payload limit. FFmpeg decodes both generated clips. The normal Debug and
SDRAM flow builds pass.

On the connected board, a real 1600x1200 JPEG was saved as PHOTO001.JPG
(85753 bytes), reread byte-for-byte, exported as `build/camera-uxga-photo.jpg`
and decoded with Pillow. A 40-frame VIDEO005.AVI recording at 1600x1200 took
9133 ms (4.38 FPS); all 4137446 payload bytes passed the recorder's SD
readback/hash checks, with zero DMA/DCMI/storage errors. Capture took 6630 ms,
SD writing 2316 ms and pacing 189 ms. This short bench run establishes the
resolution/frame-rate tradeoff; it is not a sustained performance guarantee.

VIDEO006.AVI was a separate two-frame export test. All 197823 payload bytes
passed readback; the complete 198104-byte file was exported as
`build/camera-uxga-video.avi`. Its finalized RIFF/header/index counts agree,
and FFmpeg decoded both 1600x1200 frames with `-xerror`, exit 0. A request
with unsupported width 1280 returned CAMERA_RESOLUTION before sensor
transactions, DMA or SD writes (all counters zero).

After that guard check, the final flashed Debug firmware recorded VIDEO007.AVI:
40 frames in 9066 ms (4.41 FPS), 4194330 payload bytes verified, zero errors,
and successful close/unmount. Left normal firmware running at width 1600,
frame-count setting 40, no pending request and export disabled; disconnected
the debugger and stopped the temporary server. Final report:
`build/camera-uxga-final-result.txt`. Previous flash was backed up as
`build/before-uxga-flash.bin` before the initial UXGA trial.

Evidence: `build/camera-uxga-photo-result.txt`,
`build/camera-uxga-video-result.txt`, `build/camera-uxga-export-result.txt`,
`build/camera-uxga-container-result.json`, `build/camera-uxga-decode.log`,
`build/camera-uxga-guard-result.txt` and `build/camera-uxga-host-test.txt`.
The former VGA recordings documented below describe earlier configurations.

## Invalid thermal frame and false camera-timeout fix

A live failure reproduced CAMERA_VERIFY (stage 8), CAMERA_TIMEOUT (error 4)
with normal-camera file/close results zero and all 759188 recorded JPEG bytes
verified. The thermal report showed last_error=-8 (vendor frame validation),
only 3 saved thermal frames and no SD error. The session coordinator had
aborted both workers on that one thermal error, and normal-camera finalization
then converted the peer failure into a misleading local timeout.

Thermal capture now drops/retries invalid reads within the shared deadline;
eight consecutive read errors still fail that thermal recording explicitly.
Once both cameras have started, a peer failure no longer truncates a healthy
stream. Preparation still aborts if either camera cannot become ready.
Each worker reports its own result; final Bluetooth output identifies which
stream failed. Camera errors preserve the stage where the actual failure
occurred rather than reporting the later verification stage.

The fix is flashed as build/drone-camera-peer-error-fixed.elf. A controlled
single -8 injection during thermal acquisition exercised recovery: VIDEO001.AVI,
640x480, 102 frames/10000 ms (10.2 FPS), 2151579 sample bytes verified;
T0004.AVI, 11 valid frames/10000 ms. One thermal read error/retry was counted,
both success bits completed, no session failure and no open files. This was
a deliberately injected invalid-frame return, not an additional natural fault.
Evidence: build/camera-verify-error-live.txt,
build/camera-peer-error-one-bad-frame.txt and
build/camera-peer-error-recovery-result.txt. Host tests cover independent
post-start failure handling, and Debug/Release/camera-only builds pass.


## VGA direct-MJPEG mode (hardware validated)

The user selected higher normal-camera resolution/FPS with VLC-compatible
AVI rather than direct Windows Media Player MP4 compatibility. The default
`CAMERA_FAST_MJPEG=ON` profile now configures a native SVGA sensor window
and downsizes to 640x480 JPEG, storing compressed camera frames directly
in VIDEOxxx.AVI. It avoids JPEG decoding, BGR conversion and software H.264
encoding. The 128 KiB capture bound and complete-frame validation remain.
The sensor window uses Espressif's Apache-licensed SVGA table:
https://github.com/espressif/esp32-camera/blob/master/sensors/private_include/ov2640_settings.h

The software pacing target is 67 ms per frame (up to about 15 FPS), replacing
250 ms (4 FPS). The combined hardware test achieved 95 VGA frames over
10000 ms: 9.5 FPS with thermal recording active. This is the measured result;
15 FPS remains a pacing target rather than a guaranteed rate. JPEG quality, sensor CLKRC,
DVP divider, CPU/peripheral clocks and thermal settings are preserved.
`record` still starts both cameras for the shared 10-second capture window.
The index capacity is 300 frames; mutually exclusive AVI/MP4 container state
shares stack storage. Capture/decoding/encoding/write/pacing timing totals
and the observed FPS are available in the report/final Bluetooth message.

Build `CAMERA_FAST_MJPEG=OFF` for the earlier 320x240 H.264 MP4 default.
The source changes preserve unrelated pending patrol/UWB work. The isolated
board-test image `build/drone-camera-fast-vga.elf` uses the last committed
application plus these camera changes. The later hardware validation used
the VGA firmware already running on the board, not a reflash of that isolated
image. Its source
snapshot is build/camera-fast-validation-src. Do not confuse a successful
host fixture with an actual sensor recording.

Validation: normal and isolated builds pass. A host fixture exercises the
actual JPEG parser and AVI writer with 150 VGA JPEG frames, verifies the
10-second stream timing, index, frame payloads and 300-frame limit, and
FFmpeg decodes every frame. Thermal AVI tests and command/session tests pass.


### Combined hardware recording result

VIDEO000.AVI: 640x480, 95 frames over 10000 ms (9.5 FPS), 2059766 JPEG payload
bytes verified by SD readback. Capture accounted for 4507 ms, writes/hash
work 5326 ms and pacing 207 ms. Camera, file close and unmount errors were zero.
T0012.AVI: 12 thermal frames over the same 10000 ms window, no sensor/storage
errors. The actual thermal header buffer prepared for the final write has
12 frames, scale 1000, rate 1200, implying a 10.0-second playback timeline.
Both files were closed with no open storage handles; GPS kept receiving.
Evidence: build/camera-vga-both-record-result.txt,
build/camera-vga-thermal-header.bin and build/camera-vga-record-final-state.txt.

An earlier first-write NACK at sensor startup cleared after an address-only
probe; ID 0x2642 and subsequent ordinary startups/captures succeeded. Its
original trigger is not established. This does not establish a short.

## Bluetooth recording command

Normal firmware waits for `record` over Bluetooth UART7 (115200, 8N1).
Send the word with CR, LF, CRLF, or as one message without a newline.
Matching is case-insensitive; surrounding spaces are accepted. Startup
calibrates the thermal camera but creates no video files. If it is not
ready yet, the command replies to try again shortly.

Both cameras prepare and open separate numbered files, then share one
10-second capture window. OV2640 saves VIDEOxxx.MP4 (H.264); MLX90640 saves
Txxxx.AVI (the imported uncompressed thermal format). Frame counts depend
on available CPU/SD time. Both container timelines span 10000 ms. Encoding,
closing and MP4 SD readback can finish after the capture window ends.

Wait for `REC: done; both 10-second files saved` before sending another
command. A command during preparation, recording or saving replies busy.
A failure is reported explicitly and releases the other camera from the
start barrier. Sensor calibration, clocks, shared I2C timing and palette
are preserved; see [thermal integration](thermal-integration.md).

Physical Bluetooth validation: the user sent commands, UART7 received
24 bytes with no RX errors/overflows, two sessions succeeded and one busy
command was rejected. The second session saved VIDEO008.MP4 (34 frames,
337337 encoded sample bytes verified) and T0013.AVI (10 thermal frames),
both with duration 10000 ms. GPS and audio remained healthy. Evidence:
build/record-command-bluetooth-result.txt.

The current pinned image is build/drone-record-command-final.elf.
`python -m unittest discover -s tests -p 'test_*.py'` checks UART parsing,
warmup gating, synchronization, deadline/wraparound, busy/repeat requests,
failure release, thermal AVI layout, sector-safe writes and partial-write
recovery. Debug, Release and camera-only builds pass. The dedicated
BOARD_CAMERA_TEST build retains its automatic diagnostic recording.

## Integrated application

The normal Debug build now includes the working H.264 MP4 recorder alongside
GPS, UWB, MAVLink, Bluetooth and the audio player. `CAMERA_RECORDING=ON` is the
default; `BOARD_CAMERA_TEST` remains available for camera-only diagnostics.
Build with `cmake --preset Debug` and `cmake --build build/Debug`.

The recorder waits for the Bluetooth `record` command, captures a 10-second
320x240 clip, finalizes and verifies it, then returns to idle. Bluetooth reports `CAM: recording`, `CAM: saved`, or failure.
There is no audio track in the MP4. External SDRAM is not used.

Audio and camera have separate DMA-accessible file objects. A shared storage
manager mounts the volume once and prevents unmounting while another file
is open; FatFs serializes volume operations. Audio runs at normal priority,
above the low-priority video encoder. Bluetooth messages share a transmit
mutex. The MLX90640 task records on the same command.

The combined build is `build/Debug/projects.elf`. In VS Code,
select `STM32Cube: Drone + MP4 camera (Debug)` after building Debug.
Earlier integration validation (before command triggering) saved
`0:/VIDEO000.MP4`: 40 frames over 10009 ms, all 388382 encoded sample bytes
verified by SD readback. Camera, FatFs, close and unmount results are zero.
Audio reached `AUDIO_DONE`, 231075 frames played, `FR_OK`; GPS received
10638 bytes and 77 PVT messages in the same run. All 11 tasks remain present
with 22496 bytes of FreeRTOS heap free. Evidence:
`build/integrated-sd-result.txt`. Earlier SD timeouts occurred with the card
absent; the user inserted it before this successful run.
The Media Player playback confirmation below applies to the earlier isolated
recording using the same MP4 format and encoder.
Power off the board before removing the SD card.

## Earlier automatic-recording validation

The following results describe historical diagnostic builds.

**Playback confirmed:** the user opened the actual MCU/SD-exported
camera-real-mcu.mp4 in Windows Media Player and explicitly replied
"Plays correctly". The final MP4 firmware is flashed as
build/camera-mp4-final.elf, matching build/Camera/projects.elf. Its default
boot recording, VIDEO002.MP4, completed40 frames over9978ms. All374412 encoded
sample bytes passed SD readback verification; file/close/unmount error codes
are zero. Final file size375364 bytes; write counter375368 includes patching
the4-byte mdat size. Evidence: build/camera-mp4-final-result.txt. Camera and
normal Debug builds pass and all13 host tests pass. Debug server stopped
after detach with the file closed and SD unmounted. Reset records the next
numbered40-frame MP4 clip. Older AVI attempts remain unchanged.

## Current default: direct H.264 MP4 on the H743

The uncompressed AVI attempt below was **not compatible with the user's
Windows Media Player**, despite successful FFmpeg decoding and Windows source
reader API checks. The user supplied a screenshot of VIDEO002 failure
0x8000FFFF. Actual Media Player GUI tests also rejected thermal RGB24 AVI and
a YUY2 comparison; a transcoded H.264 MP4 visibly played the room video. A
source-reader API check is not a playback test. Do not repeat the earlier
claim that RGB24 AVI is confirmed playable in that app. Timing-only AVI
changes were inconclusive and are not a validated fix.

The Camera firmware now **captures JPEG, decodes on the MCU, converts to
I420, software-encodes H.264 Baseline and writes MP4 directly to SD**. It does
not rely on a PC converter. Default file names are VIDEO000.MP4, VIDEO001.MP4,
etc., created without overwriting existing files. The older AVI files remain
unchanged and do not become MP4-compatible after flashing.

### Actual hardware results

- First default-boot record: VIDEO000.MP4, 40 actual camera frames over9982ms.
  All374238 bytes of encoded sample data passed SD readback hashes. File,
  close/unmount, camera and DMA errors are zero. Evidence:
  build/camera-mp4-first-result.txt. Final file size375190 bytes (write counter
 375194 includes patching the four-byte mdat size).
- Last H.264 access unit exported directly from the M7 encoder memory decodes
  successfully with FFmpeg, Baseline/YUV420,320x240. SPS reports level1.1,
  appropriate for the300 macroblocks in this frame. Evidence:
  build/camera-mcu-last.h264 and build/camera-mcu-h264.png.
- A second8-frame recording, VIDEO001.MP4, was completely reread from SD into
  internal SRAM and exported through ST-Link as build/camera-real-mcu.mp4.
  It is81303 bytes, 320x240 at4fps,2seconds; every one of its8 frames decodes
  with FFmpeg -xerror, exit0. No PC re-encoding was performed on this file.
  All80607 encoded sample bytes passed embedded verification. Evidence:
  build/camera-mp4-file-result.txt and build/camera-real-mcu-mp4-decode.txt.

The user stopped computer control with Esc, so no further Media Player UI
automation was performed. The actual exported board MP4 was given to the user,
who subsequently confirmed that it plays correctly in Windows Media Player.

### Defaults, memory and checks

One40-frame clip per reset, paced at up to4fps, then finalize, verify, close,
unmount and idle. Measured target performance completed40 frames in roughly
10seconds at320x240 without SDRAM. All-IDR encoding avoids motion-reference
dependencies; quality is fixedQP28. Typical observed clip size is about375KB
for10seconds, but scene content affects it. No audio is recorded.

video_mp4=1 selects MP4 (default); when it is zero, the older AVI settings are
used. video_uncompressed=1 only controls the AVI fallback. video_frames=40,
save=1, probe=0, prefix skip=0, CLKRC=80, DCMI polarities=4 remain defaults.
The short export test used8 frames in RAM; restored to40 afterward. Debug
export flag is zero by default and after the test.

MP4 uses fixed allocations:481920 bytes AXI SRAM,176KiB D2 SRAM and83224
bytes DTCM in the current build. This includes the128KiB JPEG capture buffer,
225KiB BGR buffer reused as encoder scratch,112.5KiB I420 buffer,160KiB encoder
persistent allocation,16KiB D2 diagnostic buffer and8KiB decoder workspace.
The isolated camera task has16KiB stack within the existing RTOS heap. New
encoder code uses -O2; no dynamic allocation was introduced by the codec/muxer.

The latest flashed image is build/camera-mp4-final.elf. Run all relevant host checks:

```powershell
python tests/test_camera_mp4.py
python tests/test_camera_bgr.py
python tests/test_camera_avi.py
python tests/test_camera_jpeg.py
cmake --build build/Camera
```

The2 MP4 tests cover real JPEG-to-H264/container output, box/sample bounds,
empty/over-limit clips and failed write/seek operations. Along with BGR, AVI
and JPEG suites, all13 host tests pass. The actual M7 bitstream and completed
SD MP4 were independently decoded, which also checks the ARM encoder port.

## Previous uncompressed AVI attempt (Media Player failed)


The user requested firmware output playable by ordinary PC users without
conversion or an MJPEG codec. The Camera build now defaults to **Windows
RGB24 BI_RGB uncompressed AVI**, still named VIDEOxxx.AVI. WAV is an audio
container and is not used for video. No format can guarantee every player,
but this format removes the MJPEG decoder requirement.

The working JPEG sensor/DCMI capture path is retained. On the H743, TJpgDec
R0.03 decodes each JPEG to a bottom-up, four-byte-row-aligned BGR24 DIB. The
AVI writer uses DIB stream handler, BI_RGB compression=0, 24bit pixels, 00db
frame chunks and a matching keyframe index. No H.264 encoder, external
SDRAM, heap allocation, desktop conversion or codec installation is required.
The dedicated Camera build uses 366720 bytes AXI SRAM, 16KiB D2 SRAM and
83192 bytes DTCM, including an 8KiB fixed JPEG-decoder workspace.

### Verified recording

Flashed `build/camera-compatible.elf`, matching the Camera build. Real SD file
**VIDEO002.AVI** contains 40 actual camera frames, 320x240, over 9982ms.
All 9216000 uncompressed frame bytes passed readback hashes. SD/FatFs,
close and unmount errors are zero. Expected final file size is 9217192 bytes;
bytes_written=9217416 includes rewriting the 224-byte header.
Evidence: build/camera-compatible-final-result.txt.

The final on-MCU decoded BGR frame was exported and visually inspected as
build/camera-compatible-last.png. Colors/image content are correct. A format
test clip generated through the same C decoder and AVI writer was recognized
by FFmpeg as rawvideo/bgr24 and all four frames decoded without errors.
Windows Media Foundation's built-in source reader opened and read all four
frames with subtype RGB24 (00000014-0000-0010-8000-00aa00389b71). That Windows
check used the host format test clip, not a direct desktop read of SD VIDEO002.
The source-reader API check is not an observation of every desktop player.

Default operation remains one 40-frame clip per power-up/reset at up to4fps,
then verification, close/unmount and idle. Expect about **9.2MB per10seconds**,
versus the much smaller earlier MJPEG clips. Use VIDEO002 or a subsequent
recording; VIDEO001 remains the older MJPEG format. Set
camera_snapshot_video_uncompressed=0 only to opt back into MJPEG. The saved
filename extension remains AVI in both modes; do not rename video to WAV.
Wait for VIDEO PASS, then power off before removing the card.

All 11 host tests pass (2 decoder/BGR tests covering RGB sampling, colors,
orientation, row padding, AVI headers/index and invalid inputs; 3 AVI tests;
6 JPEG tests), and the Camera build passes. Native Windows validation helper:
build/NativeVideoCheck.cs. TJpgDec sources/license notes are retained under
Drivers/Camera/tjpgd. Debug server stopped after detach; recording file is
closed and SD unmounted. Earlier sections below describe the MJPEG phase.

## Working recording verified, 2026-11-12

**Real OV2640 video now records successfully.** The user repaired the SDA/D0
connection. With the module reconnected, three repeats confirmed PB9 SDA and
PH9 D0 switch independently. The full original reserved-register preamble now
completes without errors and camera clock/sync outputs are active.

The working camera firmware is flashed. A default-boot test created
`0:/VIDEO001.AVI`: 40 actual camera JPEG frames at 320x240 over 9996ms. All
382514 bytes of JPEG payload were read back with matching per-frame hashes;
FatFs, close, unmount, DMA and DCMI errors were zero. VIDEO000.AVI was an
earlier attempt that failed verification; use VIDEO001.AVI as the verified clip.
An actual captured JPEG was exported and decoded with Pillow, showing the
camera's view of the room, not a test pattern. Evidence:
`build/camera-repaired-video001-result.txt`, `build/camera-after-repair-coupling.txt`,
`build/camera-real-frame.jpg` and `build/camera-real-frame.png`.

The final fixes beyond the physical repair were:

- VSYNC must be active LOW for this camera. Updated both dcmi.c and the IOC;
  capture default polarity value is 4 (rising PCLK, low HSYNC, low VSYNC).
- Override the Espressif JPEG profile's HREF=VSYNC option with IMAGE_MODE=10
  for separate JPEG data-valid timing.
- Capture continuously until a structurally complete JPEG is in DMA memory,
  rather than accepting the first frame interrupt as a complete image.
- Split AVI read/write calls at partial sector boundaries. Aligning only the
  application's buffer is insufficient: FatFs can consume an initial partial
  sector and then pass an unaligned interior pointer to SDMMC IDMA. This
  caused verification failure on the first variable-sized real-camera clip;
  the corrected actual-camera clip passed full payload verification.

### Current operation

The isolated Camera build now defaults to a 40-frame clip at up to 4fps,
roughly 10 seconds. Reset/power-up records one new numbered VIDEOxxx.AVI,
verifies it, closes/unmounts, and idles. It does not overwrite existing clips.
Wait for `CAMERA: VIDEO PASS ...` before powering off/removing the SD card.
Normal GPS/UWB application tasks remain disabled in this dedicated build.

Build with `cmake --build build/Camera`; launch **STM32Cube: Camera video to
SD (internal SRAM)**. The current pinned flashed image is
`build/camera-after-repair.elf`, matching `build/Camera/projects.elf`.
Set video_frames=0 for single JPEG, or 1..120 for clip length; request=1 starts
another clip without reset. Defaults: save=1, video_frames=40, probe=0,
skip_dsp_prefix=0, clockrc=80 (reported 12MHz module input, doubler enabled),
dcmi_polarities=4. External SDRAM is unused. This is a bounded, sequential
capture/write recorder with no audio; it does not guarantee high frame rates.

Camera and normal Debug builds pass, plus all 3 AVI and 6 JPEG host tests.
The next section and later diagnostic notes preserve the earlier investigation;
their unsuccessful-capture statements describe the pre-repair state, not current
recording behavior. The debug server is stopped after detach; the verified file
is closed and SD filesystem unmounted.

## Earlier status before repair, 2026-11-12

**Camera-origin video is blocked: no frames arrive at the MCU.** This is not
a working-camera claim. The SD recorder has been tested separately with an
explicitly labelled synthetic pattern. No external SDRAM is used.

- Hardware ID reads 0x2642 and reversible writes/readbacks pass in sensor and
  DSP banks. Normal prefix fails after DSP 2C=FF; omitting it permits remaining
  writes to complete but does not produce image signals.
- The user reports the module has a 12 MHz crystal. Oscillation has not been
  measured. CLKRC=80 (documented frequency doubler) was tested with the prefix
  omitted: JPEG initialization completed, then DCMI timed out after 3 seconds,
  with zero bytes, zero frame events and zero GPIO transitions.
- A separate probe read normal sensor mode (COM2=02), live-video option
  (COM3=3C), CLKRC=00, COM7=00, COM10=00, DSP bypass=00, divider=04,
  IMAGE_MODE=09, DSP RESET=00 and size registers 78/44/00 after the supplied
  RGB565 table. Explicitly bypassing DSP, releasing reset, disabling standby
  and PCLK gating, and trying CLKRC 00/01/03/07/80/83 produced zero sampled
  PCLK/HREF/VSYNC edges in six 500ms windows. Inputs were temporarily configured
  as ordinary digital inputs, then restored to DCMI AF13. This does not prove
  a physical short or absence of an oscillator, or validate the incomplete
  startup configuration.
- Using the real H743/SDMMC/FatFs path, `TEST000.AVI` and `TEST001.AVI` each
  contain eight labelled **SD STORAGE TEST - NOT CAMERA** frames. Every JPEG
  payload was read back and its FNV-1a hash compared. All SD/FatFs/close/unmount
  results were zero. The second file was exported through ST-Link after an SD
  reread as `build/sd-storage-test.avi`: 52008 bytes, 320x240, eight JPEG frames,
  1999ms duration. Independent host parsing checked RIFF/list sizes, frame
  chunks and index size, and Pillow decoded all eight frames successfully.

## Recorder behavior

The opt-in recorder creates the next unused `VIDEO000.AVI` through
`VIDEO999.AVI` using FA_CREATE_NEW. It validates a first 320x240 JPEG before
initializing SD or creating a file. It then alternates snapshot capture and
SD writing using the 128KiB AXI buffer and 4KiB aligned I/O buffer. There is
no audio, double buffering, external SDRAM dependency or guaranteed real-time
frame rate. The recorder paces at up to 4fps; header timing uses measured
recording duration, so slower captures are reflected in playback rate.

Clips are bounded to 120 frames (40 is a convenient roughly 10-second test).
At completion, it writes an AVI 1.0 keyframe index, fixes header sizes/counts,
syncs/closes, reopens, and reads every JPEG payload back to compare its hash.
The host tests separately validate container structure and JPEG decoding.
Capture failure after saved frames attempts to finalize the shorter clip but
still reports failure. SD errors or power loss can leave a partial AVI; only
VIDEO PASS means the requested clip completed and payload verification passed.

`camera_avi.c` follows Microsoft's AVI RIFF layout:
https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference

## Build and debugger controls

Use the existing Camera build/launch in camera-snapshot.md. All flags must be
set after C initialization but before CameraSnapshot_Task, or before setting
camera_snapshot_request=1 for an idle task:

```gdb
set camera_snapshot_probe_registers = 0
set camera_snapshot_skip_dsp_prefix = 1
set camera_snapshot_clockrc = 0x80
set camera_snapshot_video_frames = 40
set camera_snapshot_save_to_sd = 1
set camera_snapshot_request = 1
```

These settings are a tested **unsuccessful camera bring-up attempt**, not a
validated recipe. With current hardware they time out before accessing SD.
Video_frames=0 preserves the single-JPEG behavior. Clockrc=FFFFFFFF preserves
the profile clock setting. Default boot behavior remains single JPEG with the
original prefix and no CLKRC override. Normal application tasks remain disabled
in this isolated Camera build.

Probe_registers=6 skips camera acquisition and records the labelled synthetic
pattern to a TESTxxx.AVI filename. Use video_frames=8 and save_to_sd=1 for the
storage test. The pattern is constant; no camera image is represented. In this
mode only, captured_bytes can hold the size of an AVI reread into internal
SRAM for ST-Link export, rather than a camera DMA byte count.

## Verification and evidence

```powershell
python tests/test_camera_avi.py
python tests/test_camera_jpeg.py
cmake --build build/Camera
```

Three AVI tests pass: real JPEG decoding and odd/even chunk/index layout,
120-frame limit/empty clip rejection, and write/seek failure propagation.
Six JPEG parser tests pass. Camera build passes. Evidence:

- build/camera-output-clock-result.txt
- build/camera-video-first-result.txt
- build/camera-video-storage-result.txt
- build/camera-video-storage-export-result.txt
- build/sd-storage-test.avi (export of the actual SD TEST001.AVI)

Pinned last flashed image: build/camera-video-storage-test.elf. Left idle with
request/save/video flags zero; probe=0, prefix skip=1 and CLKRC=80 in RAM.
Debug server stopped; SD file closed and filesystem unmounted. Reset restores
default flags. No normal GPS/UWB application restoration was requested.

## DCMI/DMA audit and SDA/D0 coupling (2026-11-12)

The actual DMA1 Stream0 copied and verified 4096 bytes from AXI SRAM into
both the capture buffer at 0x24000000 and a diagnostic D2 buffer at 0x30000000.
This tests the DCMI stream, rather than inferring accessibility from SDMMC.
DMA request on the live stream is DCMI (DMAMUX CCR=4B), PAR=48020028,
M0AR equals the selected SRAM buffer, data sizes word/word, increment enabled
for memory. D-cache remains disabled. Raw RGB continuous capture across all
8 PCLK/HREF/VSYNC polarity combinations started successfully but retained all
4096 words in NDTR, with no observed DCMI events or DMA error. Evidence:
build/camera-raw-dma-result.txt. Probe7 executes these tests; current source
reports timeout when every trial receives zero bytes (initial diagnostic
reported only successful execution, not camera capture).

A direct DCMI input-path test holds camera RESET_N low/PWDN high and uses
weak internal pull-up/down changes while retaining AF13 on all DCMI inputs.
Slow generated pixel/sync waveforms produced 512 bytes through the actual
DCMI->DMA->AXI buffer. Pattern A5 yielded 128 exact A5A5A5A5 words, and FF
also passed. Patterns 00 and 5A yielded 01 and 5B: D0 high despite its weak
pull-down. CPU GPIO reads match the DCMI data, so this mismatch is not introduced
by DMA packing or JPEG parsing. An earlier GPIO-output injection attempt was
inconclusive because it changed the AF input routing; use only AF13 weak-pull
results. These are low-speed electrical/peripheral tests, not camera images
or proof of high-speed signal quality.

The user then explicitly confirmed camera removal and board power/ST-Link.
The same D0-high behavior persisted. A 13-signal input-pull-up/open-drain
matrix, now INCLUDING PB8/PB9 alongside DCMI, showed reciprocal coupling:
PB9 low drives PH9 low; PH9 low drives PB9 low. Follow-up isolated only these
two pins with I2C1 PE disabled, GPIO input pull-ups, and one open-drain low
source at a time. All THREE rounds gave:

| State | PB9 SDA | PH9 D0 |
|---|---|---|
| Both released | 1 | 1 |
| PB9 driven low | 0 | 0 |
| PB9 released | 1 | 1 |
| PH9 driven low | 0 | 0 |
| PH9 released | 1 | 1 |

This establishes unintended conductive coupling on the remaining board/wiring,
with the camera removed. It does not locate the bridge or establish a measured
zero-ohm resistance. Schematic has separate nets: CAM_SDA J1 pad3/R2 pad2/U1
PB9 pin168, and DCMI_D0 J1 pad4/U1 PH9 pin86. Adjacent J1 pad3/pad4 soldering
or adapter/jumper connections are the first inspection targets. This could
explain why activating camera data output pulls SDA low. Root cause/location
and successful capture still require repair and retest.

KiCad DRC on the existing PCB, without modifying/saving it, reports 253 total
violations but zero shorting_items; no SDA/D0 short is represented in the PCB
file. Do not describe the entire PCB as DRC-clean. Report:
build/camera-crossbus-board-drc.json. Matrix and repeated evidence:
build/camera-bare-crossbus-result.txt and
build/camera-bare-sda-d0-repeat-result.txt.

Earlier pin-coupling tests did not include SCCB and DCMI data together, so
those tests did not exclude this connection. The earlier hardware conclusion
was incomplete. Source builds successfully after diagnostic result semantics
were corrected. Last flashed ELF is build/camera-dcmi-selftest.elf (before
that reporting-only correction); source build/Camera/projects.elf is newer.
Camera remains disconnected by user, request/save/probe flags zero, MCU reset
and powerdown outputs held low/high respectively. PH9 restored to AF13 with
no pull; debug server stopped after detach. Awaiting physical inspection/repair.

## Playback compatibility check

The user supplied the actual SD file E:/VIDEO001.AVI after a player reported
unsupported format. Its 383730-byte RIFF size, 40 JPEG frame chunks, movi list,
stream timing and index sizes are consistent. Pillow decoded all 40 frames.
FFmpeg independently recognized MJPEG/yuvj422p, 320x240, approximately4fps,
10seconds and decoded all40 with exit0 and no decode errors. The recorded
AVI is valid for FFmpeg; the user's player issue appears to be codec/container
compatibility, not evidence that no camera video was recorded. The particular
player and its installed codecs were not inspected.

Created H.264 Baseline/yuv420p MP4 at4fps, faststart, 40frames/10seconds:
E:/VIDEO001-compatible.mp4 and build/VIDEO001-compatible.mp4 (438341bytes).
FFmpeg decoded the resulting MP4 with -xerror and returned0 with empty error
output. Original AVI was not modified. Logs: build/video001-decode-log.txt and
build/video001-convert-log.txt. For conversion only, imageio-ffmpeg0.8.1 was
installed locally under build/video-tools; no global codec pack or system
player settings were changed. Firmware still records MJPEG AVI, not H.264.
