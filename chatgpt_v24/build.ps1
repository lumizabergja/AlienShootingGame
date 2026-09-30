$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path '.').Path
$stage = Join-Path $repo 'chatgpt_v24\stage'
$work = Join-Path $stage 'work'
$third = Join-Path $stage 'third_party'
$out = Join-Path $stage 'out'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage,$work,$third,$out | Out-Null

$chunks = Get-ChildItem (Join-Path $repo 'chatgpt_v24\payload\*.txt') | Sort-Object Name
$b64 = (($chunks | ForEach-Object { (Get-Content $_.FullName -Raw).Trim() }) -join '')
$zip = Join-Path $stage 'payload.zip'
[IO.File]::WriteAllBytes($zip, [Convert]::FromBase64String($b64))
Expand-Archive -LiteralPath $zip -DestinationPath $work -Force

$src = Join-Path $work 'source'
if (!(Test-Path (Join-Path $src 'main.cpp'))) { throw 'V24 source reconstruction failed.' }

$sl = Join-Path $third 'Streamline'
git clone --depth 1 --branch v2.14.1 https://github.com/NVIDIA-RTX/Streamline.git $sl
if ($LASTEXITCODE -ne 0) { throw 'Streamline clone failed.' }

$nvof = Join-Path $third 'nvof'
New-Item -ItemType Directory -Force -Path $nvof | Out-Null
$nvBase = 'https://raw.githubusercontent.com/mbucchia/Optical-Flow-SDK/54e68293b4898a530bc07e4d7df71efbc5d30f9b/NvOFInterface'
Invoke-WebRequest "$nvBase/nvOpticalFlowCommon.h" -OutFile (Join-Path $nvof 'nvOpticalFlowCommon.h')
Invoke-WebRequest "$nvBase/nvOpticalFlowD3D12.h" -OutFile (Join-Path $nvof 'nvOpticalFlowD3D12.h')

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (!$vs) { throw 'Visual Studio C++ toolchain not found.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

Push-Location $src
& rc.exe /nologo /fo (Join-Path $out 'app.res') 'app.rc'
if ($LASTEXITCODE -ne 0) { throw "rc.exe failed: $LASTEXITCODE" }
Pop-Location

$exe = Join-Path $out 'DX12-DLSS-FG-V24-ZERO-RASTER-PARALLEL-COPY.exe'
$cpp = @(
    (Join-Path $src 'main.cpp'),
    (Join-Path $src 'dlss_bridge.cpp'),
    (Join-Path $src 'nvof_bridge.cpp'),
    (Join-Path $src 'wgc_capture.cpp')
)
$clArgs = @(
    '/nologo','/std:c++20','/EHsc','/O2','/MT','/Zc:__cplusplus',
    '/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
    "/I$src", "/I$sl\include", "/I$nvof",
    "/Fe:$exe"
) + $cpp + @(
    (Join-Path $out 'app.res'),
    '/link','/INCREMENTAL:NO','/OPT:REF','/OPT:ICF','/SUBSYSTEM:WINDOWS',
    'd3d11.lib','d3d12.lib','dxgi.lib','d3dcompiler.lib',
    'user32.lib','gdi32.lib','comctl32.lib',
    'wintrust.lib','crypt32.lib','ole32.lib','runtimeobject.lib','windowsapp.lib',
    'shell32.lib','shcore.lib','dwmapi.lib'
)
& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed: $LASTEXITCODE" }
if (!(Test-Path $exe)) { throw 'Compiler returned success but EXE is missing.' }

Get-FileHash $exe -Algorithm SHA256 | Format-List
Write-Host "V24_EXE=$exe"
