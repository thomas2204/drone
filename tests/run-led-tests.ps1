$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build/led-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& gcc -std=c11 -Wall -Wextra -Werror "-I$PSScriptRoot/led_stubs" "$root/Core/Src/led_strip.c" "$PSScriptRoot/led_startup_test.c" -o "$output/led_startup_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'LED startup test compilation failed' }
& "$output/led_startup_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'LED startup tests failed' }
