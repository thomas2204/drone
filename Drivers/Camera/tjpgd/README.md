# TJpgDec R0.03

Downloaded from ChaN's official distribution:
https://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip

Project: https://elm-chan.org/fsw/tjpgd/

Original copyright and redistribution terms remain in tjpgd.c. Configuration
uses RGB888 output and JD_FASTDECODE=1, with an 8KiB fixed workspace in the
application wrapper. The header uses standard C11 stdint.h on Windows as well
as the MCU, avoiding the original Windows unsigned-long typedef conflict.
The published 2023 grayscale-only patch is not needed for this RGB888 build.
There is no heap allocation. The camera's supported baseline JPEG output is
decoded to bottom-up BGR24 for BI_RGB AVI playback without an MJPEG codec.
