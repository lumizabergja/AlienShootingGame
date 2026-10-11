param([Parameter(Mandatory=$true)][string]$Src)
$ErrorActionPreference = 'Stop'

$logger = Join-Path $Src 'src/Logger.cpp'
$sl = Join-Path $Src 'src/StreamlineRuntime.cpp'

$loggerText = Get-Content $logger -Raw
$oldDump = 'MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr, nullptr);'
$newDump = 'static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr, nullptr);'
if (-not $loggerText.Contains($oldDump) -and -not $loggerText.Contains($newDump)) { throw 'Expected MiniDump flags expression not found.' }
$loggerText = $loggerText.Replace($oldDump, $newDump)
Set-Content -Path $logger -Value $loggerText -Encoding UTF8 -NoNewline

$slText = Get-Content $sl -Raw
$oldVersion = 'slInit(pref, SDK_VERSION);'
$newVersion = 'slInit(pref, sl::kSDKVersion);'
if (-not $slText.Contains($oldVersion) -and -not $slText.Contains($newVersion)) { throw 'Expected slInit SDK version expression not found.' }
$slText = $slText.Replace($oldVersion, $newVersion)
Set-Content -Path $sl -Value $slText -Encoding UTF8 -NoNewline

if (-not (Select-String -Path $logger -Pattern 'static_cast<MINIDUMP_TYPE>' -Quiet)) { throw 'MiniDump fix validation failed.' }
if (-not (Select-String -Path $sl -Pattern 'slInit\(pref, sl::kSDKVersion\)' -Quiet)) { throw 'Streamline SDK version fix validation failed.' }
Write-Host 'Fresh compile fixes applied and validated.'
