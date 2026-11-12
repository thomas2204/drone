# minih264 Cortex-M7 port

Upstream: https://github.com/lieff/minih264
Pinned commit: b0baea7a80ef9d12da97301dd1099b8791b5ba43

minih264e.h and the CC0 LICENSE are retained. The sole header port change
adds a bare-metal GNU ARM platform branch using caller-declared little-endian
byte order. Cortex-M7 uses the plain-C implementation plus upstream ARM DSP
instructions; it has neither NEON nor SSE. The application disables SVC,
threads and denoise, uses 320x240 YUV420, fastest mode, QP28 and all-IDR frames.

Persistent encoder memory is a fixed160KiB allocation in D2 SRAM. The existing
230400-byte BGR decode buffer is reused as the encoder's scratch/output after
conversion to a separate115200-byte I420 buffer. H264E_sizeof reports182607
scratch bytes on the actual target; no heap or external SDRAM is used. Encoder
source compiles with -O2 even in the diagnostic build. Upstream currently
emits one unused static denoise-helper warning with denoise disabled.

The portable MP4 writer in Core/Src/camera_mp4.c packages length-prefixed AVC
samples, SPS/PPS in avcC, bounded sample tables and measured average timing.
Only the encoder's all-IDR output is accepted, so every sample is independently
decodable. MP4 requires successful finalization; interrupted power/file errors
can leave an incomplete file. The SD path reads every encoded sample back and
compares its hash before reporting VIDEO PASS.
