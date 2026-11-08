param([string]$Configuration = "Release")

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $projectRoot "build/win64"
$distribution = Join-Path $projectRoot "dist"

& cmake -S $projectRoot -B $buildDir -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
& cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Windows compilation failed" }

$app = Join-Path $buildDir "$Configuration/FrameBridgeNative.exe"
if (!(Test-Path $app)) { throw "Expected output was not created: $app" }
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
$zipPath = Join-Path $distribution "FrameBridgeNative-CaptureProbe-Windows-x64.zip"
Compress-Archive -Path $app -DestinationPath $zipPath -Force
Write-Host "Build passed: $app"
Write-Host "Ready to download after Windows compilation: $zipPath"

