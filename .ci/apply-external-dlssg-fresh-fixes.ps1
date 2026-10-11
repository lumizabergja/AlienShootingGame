param([Parameter(Mandatory=$true)][string]$Src)
$ErrorActionPreference = 'Stop'

$logger = Join-Path $Src 'src/Logger.cpp'
$sl = Join-Path $Src 'src/StreamlineRuntime.cpp'
$app = Join-Path $Src 'src/App.cpp'
$cmake = Join-Path $Src 'CMakeLists.txt'

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

$appText = Get-Content $app -Raw
foreach ($id in @('IDC_LIST','IDC_REFRESH','IDC_MULT','IDC_RADIUS','IDC_START','IDC_STATUS')) {
    $old = "(HMENU)$id"
    $new = "reinterpret_cast<HMENU>(static_cast<INT_PTR>($id))"
    $appText = $appText.Replace($old, $new)
}

# CreateWindow sends WM_SIZE before these child HWNDs are created. Force one real
# layout pass immediately after child creation so none remain at the initial 0x0 size.
if (-not $appText.Contains('GetClientRect(hwnd_,&rc); Layout(rc.right-rc.left,rc.bottom-rc.top);')) {
    $pattern = 'for\(HWND h:\{list_,refresh_,mult_,radius_,start_,status_\}\) setFont\(h,font_\);\s*RefreshWindows\(\); SetTimer\(hwnd_,1,500,nullptr\); ShowWindow\(hwnd_,show\); UpdateWindow\(hwnd_\); return true;'
    $replacement = "for(HWND h:{list_,refresh_,mult_,radius_,start_,status_}) setFont(h,font_);`r`n    RECT rc{}; GetClientRect(hwnd_,&rc); Layout(rc.right-rc.left,rc.bottom-rc.top);`r`n    RefreshWindows(); UpdateStatus(); SetTimer(hwnd_,1,500,nullptr); ShowWindow(hwnd_,show); UpdateWindow(hwnd_); return true;"
    $newAppText = [regex]::Replace($appText,$pattern,$replacement,1)
    if ($newAppText -eq $appText) { throw 'Expected App::Create child-control tail not found.' }
    $appText = $newAppText
}
Set-Content -Path $app -Value $appText -Encoding UTF8 -NoNewline

$cmakeText = Get-Content $cmake -Raw
$cmakeText = $cmakeText.Replace('/W4 /permissive-', '/W4 /WX /permissive-')
Set-Content -Path $cmake -Value $cmakeText -Encoding UTF8 -NoNewline

if (-not (Select-String -Path $logger -Pattern 'static_cast<MINIDUMP_TYPE>' -Quiet)) { throw 'MiniDump fix validation failed.' }
if (-not (Select-String -Path $sl -Pattern 'slInit\(pref, sl::kSDKVersion\)' -Quiet)) { throw 'Streamline SDK version fix validation failed.' }
if (Select-String -Path $app -Pattern '\(HMENU\)IDC_' -Quiet) { throw 'HMENU warning cleanup failed.' }
if (-not (Select-String -Path $app -SimpleMatch 'GetClientRect(hwnd_,&rc); Layout(rc.right-rc.left,rc.bottom-rc.top);' -Quiet)) { throw 'Initial child-control layout fix validation failed.' }
if (-not (Select-String -Path $cmake -Pattern '/W4 /WX /permissive-' -Quiet)) { throw 'Warnings-as-errors validation failed.' }
Write-Host 'Fresh compile/API corrections applied; initial child-control layout fixed; /W4 /WX enabled.'
