# Build the F103 beacon using its checked-in CubeIDE source manifest, without
# depending on the absolute include paths in CubeIDE's generated makefiles.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$peerRoot = Join-Path $projectRoot 'UWB-beacon-CMake-version'
$output = Join-Path $projectRoot 'build/beacon-pdoa'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$includes = @('Core/Inc', 'Core/Inc/dwt_uwb_driver',
    'Core/Inc/dwt_uwb_driver/dw3000', 'Core/Inc/dwt_uwb_driver/dw3720',
    'Core/Inc/dwt_uwb_driver/lib/qmath', 'Core/Inc/dwt_uwb_driver/lib/qmath/include',
    'Drivers/STM32F1xx_HAL_Driver/Inc', 'Drivers/STM32F1xx_HAL_Driver/Inc/Legacy',
    'Drivers/CMSIS/Device/ST/STM32F1xx/Include', 'Drivers/CMSIS/Include') |
    ForEach-Object { '-I' + (Join-Path $peerRoot $_) }
$objects = @()
foreach ($line in Get-Content "$peerRoot/Debug/objects.list") {
    $relative = $line.Trim().Trim('"') -replace '^\./', ''
    if (-not $relative) { continue }
    $sourceRelative = $relative -replace '\.o$', '.c'
    if ($relative -like 'Core/Startup/*') { $sourceRelative = $relative -replace '\.o$', '.s' }
    $object = Join-Path $output $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
    & arm-none-eabi-gcc (Join-Path $peerRoot $sourceRelative) -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xE -c @includes -O0 -ffunction-sections -fdata-sections -Wall --specs=nano.specs -mfloat-abi=soft -mthumb -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $sourceRelative" }
    $objects += $object
}
& arm-none-eabi-gcc -o "$output/Beacon.elf" @objects -mcpu=cortex-m3 "-T$peerRoot/STM32F103VETX_FLASH.ld" --specs=nosys.specs "-Wl,-Map=$output/Beacon.map" '-Wl,--gc-sections' '-Wl,--print-memory-usage' -static --specs=nano.specs -mfloat-abi=soft -mthumb '-Wl,--start-group' -lc -lm '-Wl,--end-group'
if ($LASTEXITCODE -ne 0) { throw 'Beacon link failed' }
& arm-none-eabi-objcopy -O binary "$output/Beacon.elf" "$output/Beacon.bin"
if ($LASTEXITCODE -ne 0) { throw 'Beacon binary generation failed' }
& arm-none-eabi-size "$output/Beacon.elf"
if ($LASTEXITCODE -ne 0) { throw 'Beacon size inspection failed' }
