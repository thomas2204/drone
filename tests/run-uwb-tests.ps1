$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build/uwb-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& gcc -std=c11 -Wall -Wextra -Werror "-I$root/Core/Inc" "$root/Core/Src/uwb_protocol.c" "$PSScriptRoot/uwb_protocol_test.c" -o "$output/uwb_protocol_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB test compilation failed' }
& "$output/uwb_protocol_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB tests failed' }
& gcc -std=c11 -Wall -Wextra -Werror -O2 -flto -fwhole-program -ffunction-sections -fdata-sections "-I$PSScriptRoot/uwb_stubs" "-I$root/Core/Inc" "-I$root/Drivers/UWB" "-I$root/Drivers/UWB/dw3000" "$root/Core/Src/uwb_range.c" "$PSScriptRoot/uwb_startup_test.c" "-Wl,--gc-sections" -o "$output/uwb_startup_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB startup test compilation failed' }
& "$output/uwb_startup_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB startup tests failed' }
& gcc -std=c11 -Wall -Wextra -Werror "-I$root/Drivers/UWB" "-I$root/Drivers/UWB/lib/qmath/include" "$root/Drivers/UWB/deca_rsl.c" "$root/Drivers/UWB/lib/qmath/src/qmath.c" "$PSScriptRoot/uwb_rssi_test.c" -lm -o "$output/uwb_rssi_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB RSSI test compilation failed' }
& "$output/uwb_rssi_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'UWB RSSI tests failed' }


