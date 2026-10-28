param([string]$Compiler = "gcc")
$ErrorActionPreference = "Stop"
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $compilerTarget = & $Compiler -dumpmachine
    if ($LASTEXITCODE -ne 0) { throw "Cannot query C compiler" }
    $flags = @("-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-address-of-packed-member", "-ICore/Inc", "-IMAVLink")
    # MinGW's default MS layout otherwise pads MAVLink's yaw extension to offset 64.
    if ($compilerTarget -match "mingw") { $flags += "-mno-ms-bitfields" }
    $cases = @(
        @{ Name = "gps_nmea"; Sources = @("Core/Src/gps_nmea.c") },
        @{ Name = "gps_ubx"; Sources = @("Core/Src/gps_ubx.c") },
        @{ Name = "gps_input"; Sources = @("Core/Src/gps_input.c", "Core/Src/gps_ubx.c") }
    )
    foreach ($case in $cases) {
        $output = "build/$($case.Name)_test.exe"
        & $Compiler @flags "tests/$($case.Name)_test.c" @($case.Sources) -lm -o $output
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name): compilation failed" }
        & "./$output"
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name): test failed" }
    }
} finally {
    Pop-Location
}
