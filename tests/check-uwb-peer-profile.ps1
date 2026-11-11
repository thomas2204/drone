$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build/uwb-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$peer = Get-Content "$root/UWB-beacon-CMake-version/Core/Src/beacon_twr.c" -Raw
$drone = Get-Content "$root/Core/Src/uwb_range.c" -Raw
$pattern = 'static\s+dwt_config_t\s+config\s*=\s*\{(?<body>[\s\S]*?)\};'
$peerMatch = [regex]::Match($peer, $pattern)
$droneMatch = [regex]::Match($drone, $pattern)
if (-not $peerMatch.Success -or -not $droneMatch.Success) { throw 'Could not locate both radio configurations' }
# Compile the actual initializer bodies against the project's driver types.
# The peer files are only read; generated test files stay under build/.
$source = @"
#include "deca_device_api.h"
#include <stdio.h>
static dwt_config_t peer = {$($peerMatch.Groups['body'].Value)};
static dwt_config_t drone = {$($droneMatch.Groups['body'].Value)};
#define CHECK(field) do { if (peer.field != drone.field) { printf("Mismatch: %s\n", #field); return 1; } } while (0)
int main(void) {
    CHECK(chan); CHECK(txPreambLength); CHECK(rxPAC); CHECK(txCode); CHECK(rxCode);
    CHECK(sfdType); CHECK(dataRate); CHECK(phrMode); CHECK(phrRate); CHECK(sfdTO);
    CHECK(stsMode); CHECK(stsLength); CHECK(pdoaMode);
    puts("All 13 drone/F103 radio configuration fields match.");
    return 0;
}
"@
$source | Set-Content "$output/peer_profile_test.c"
& gcc -std=c11 -Wall -Wextra -Werror "-I$root/Drivers/UWB" "$output/peer_profile_test.c" -o "$output/peer_profile_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Peer profile test compilation failed' }
& "$output/peer_profile_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Drone and F103 radio profiles differ' }
