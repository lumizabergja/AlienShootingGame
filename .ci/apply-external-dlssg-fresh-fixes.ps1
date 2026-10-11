param([Parameter(Mandatory=$true)][string]$Src)
$ErrorActionPreference = 'Stop'

$logger = Join-Path $Src 'src/Logger.cpp'
$sl = Join-Path $Src 'src/StreamlineRuntime.cpp'
$app = Join-Path $Src 'src/App.cpp'
$dda = Join-Path $Src 'src/DdaPipeline.cpp'
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

# Prefer DuplicateOutput1 (newest/lowest-overhead form), but some current Windows/driver
# combinations expose IDXGIOutput5 yet reject DuplicateOutput1 with DXGI_ERROR_UNSUPPORTED.
# Fall back to standard IDXGIOutput1::DuplicateOutput on the same adapter/device. This is
# still native GPU-side Desktop Duplication and keeps the exact same D3D11->shared-D3D12 path.
$ddaText = Get-Content $dda -Raw
if (-not $ddaText.Contains('DuplicateOutput1 failed; trying IDXGIOutput1::DuplicateOutput')) {
    $oldFunc = @'
bool DdaPipeline::CreateDuplication(std::wstring& error) {
    duplication_.Reset();
    DXGI_FORMAT formats[]={kCaptureFormat};
    HRESULT hr=output_->DuplicateOutput1(device11_.Get(),0,1,formats,&duplication_);
    if(!ok(hr,L"IDXGIOutput5::DuplicateOutput1",error)) return false;
    DXGI_OUTDUPL_DESC dd{}; duplication_->GetDesc(&dd);
    if(dd.ModeDesc.Format!=kCaptureFormat) {
        error=L"Desktop Duplication did not provide BGRA8. Disable Windows HDR for this low-latency build.";
        duplication_.Reset(); return false;
    }
    logx::Info("Desktop Duplication active: %ux%u format=%d",dd.ModeDesc.Width,dd.ModeDesc.Height,(int)dd.ModeDesc.Format);
    return true;
}
'@
    $newFunc = @'
bool DdaPipeline::CreateDuplication(std::wstring& error) {
    duplication_.Reset();
    DXGI_FORMAT formats[]={kCaptureFormat};
    HRESULT hr=output_->DuplicateOutput1(device11_.Get(),0,1,formats,&duplication_);
    bool usedDuplicateOutput1=SUCCEEDED(hr);

    if(FAILED(hr)) {
        logx::Warn("DuplicateOutput1 failed; trying IDXGIOutput1::DuplicateOutput (hr=0x%08X)", static_cast<unsigned>(hr));
        duplication_.Reset();
        ComPtr<IDXGIOutput1> output1;
        HRESULT qhr=output_.As(&output1);
        if(FAILED(qhr) || !output1) {
            error=L"DuplicateOutput1 failed ("+HrText(hr)+L") and IDXGIOutput1 is unavailable ("+HrText(qhr)+L").";
            logx::Error("Desktop Duplication fallback unavailable: DuplicateOutput1=0x%08X QI(Output1)=0x%08X", static_cast<unsigned>(hr), static_cast<unsigned>(qhr));
            return false;
        }

        HRESULT legacyHr=output1->DuplicateOutput(device11_.Get(),&duplication_);
        if(FAILED(legacyHr)) {
            error=L"DXGI Desktop Duplication is unavailable. DuplicateOutput1: "+HrText(hr)+L"; DuplicateOutput: "+HrText(legacyHr);
            logx::Error("Both DDA entry points failed: DuplicateOutput1=0x%08X DuplicateOutput=0x%08X", static_cast<unsigned>(hr), static_cast<unsigned>(legacyHr));
            return false;
        }
    }

    DXGI_OUTDUPL_DESC dd{}; duplication_->GetDesc(&dd);
    if(dd.ModeDesc.Format!=kCaptureFormat) {
        error=L"Desktop Duplication did not provide BGRA8. Disable Windows HDR for this low-latency build.";
        duplication_.Reset(); return false;
    }
    logx::Info("Desktop Duplication active via %s: %ux%u format=%d", usedDuplicateOutput1 ? "DuplicateOutput1" : "DuplicateOutput", dd.ModeDesc.Width,dd.ModeDesc.Height,(int)dd.ModeDesc.Format);
    return true;
}
'@
    if (-not $ddaText.Contains($oldFunc)) { throw 'Expected original CreateDuplication implementation not found.' }
    $ddaText = $ddaText.Replace($oldFunc,$newFunc)
}
Set-Content -Path $dda -Value $ddaText -Encoding UTF8 -NoNewline

$cmakeText = Get-Content $cmake -Raw
$cmakeText = $cmakeText.Replace('/W4 /permissive-', '/W4 /WX /permissive-')
Set-Content -Path $cmake -Value $cmakeText -Encoding UTF8 -NoNewline

if (-not (Select-String -Path $logger -Pattern 'static_cast<MINIDUMP_TYPE>' -Quiet)) { throw 'MiniDump fix validation failed.' }
if (-not (Select-String -Path $sl -Pattern 'slInit\(pref, sl::kSDKVersion\)' -Quiet)) { throw 'Streamline SDK version fix validation failed.' }
if (Select-String -Path $app -Pattern '\(HMENU\)IDC_' -Quiet) { throw 'HMENU warning cleanup failed.' }
if (-not (Select-String -Path $app -SimpleMatch 'GetClientRect(hwnd_,&rc); Layout(rc.right-rc.left,rc.bottom-rc.top);' -Quiet)) { throw 'Initial child-control layout fix validation failed.' }
if (-not (Select-String -Path $dda -SimpleMatch 'DuplicateOutput1 failed; trying IDXGIOutput1::DuplicateOutput' -Quiet)) { throw 'DDA compatibility fallback validation failed.' }
if (-not (Select-String -Path $dda -SimpleMatch 'output1->DuplicateOutput(device11_.Get(),&duplication_)' -Quiet)) { throw 'Legacy DDA fallback call validation failed.' }
if (-not (Select-String -Path $cmake -Pattern '/W4 /WX /permissive-' -Quiet)) { throw 'Warnings-as-errors validation failed.' }
Write-Host 'Fresh compile/API corrections applied; UI layout fixed; DuplicateOutput1->DuplicateOutput GPU fallback added; /W4 /WX enabled.'
