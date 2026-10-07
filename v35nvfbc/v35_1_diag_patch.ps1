$ErrorActionPreference = 'Stop'
$path = 'v35nvfbc/source/nvfbc_cuda_capture.cpp'
$text = Get-Content $path -Raw

$pattern = '(?s)    u32 sdkVersion = 0;.*?    impl_->nvfbc\.setGlobalFlags\(NVFBC_GLOBAL_FLAGS_NO_INITIAL_REFRESH\);'
$replacement = @'
    u32 sdkVersion = 0;
    const int sdkResult = impl_->nvfbc.getSdkVersion(&sdkVersion);
    if (sdkResult != NVFBC_SUCCESS) {
        error = L"NvFBC_GetSDKVersion failed: " + NvFbcResult(sdkResult);
        Shutdown(); return false;
    }

    auto queryStatus = [&](NvFBCStatusEx& out) -> int {
        out = {};
        out.version = NvFbcStructVersion(sizeof(out), 2);
        out.adapterIdx = dxgiAdapterIndex;
        int r = impl_->nvfbc.getStatusEx(&out);
        if (r != NVFBC_SUCCESS) {
            out = {};
            out.version = NvFbcStructVersion(sizeof(out), 1);
            out.adapterIdx = dxgiAdapterIndex;
            r = impl_->nvfbc.getStatusEx(&out);
        }
        return r;
    };

    NvFBCStatusEx statusBefore{};
    const int statusBeforeResult = queryStatus(statusBefore);
    const bool captureBefore = statusBeforeResult == NVFBC_SUCCESS && ((statusBefore.flags & 1u) != 0u);

    // V35.1: always ask the official driver entry point to enable NvFBC.
    // The executable is linked with requireAdministrator so this call runs elevated.
    const int enableResult = impl_->nvfbc.enable(NVFBC_STATE_ENABLE);

    NvFBCStatusEx status{};
    const int sr = queryStatus(status);
    const bool captureAfter = sr == NVFBC_SUCCESS && ((status.flags & 1u) != 0u);

    impl_->nvfbc.setGlobalFlags(NVFBC_GLOBAL_FLAGS_NO_INITIAL_REFRESH);
'@

$updated = [regex]::Replace($text, $pattern, $replacement, 1)
if ($updated -eq $text) { throw 'V35.1 status/enable patch target was not found.' }
$text = $updated

$old = '        error = L"NvFBC_CreateEx(NvFBCCuda) failed: " + NvFbcResult(cr); Shutdown(); return false;'
$new = @'
        std::wostringstream diag;
        diag << L"NvFBC_CreateEx(NvFBCCuda) failed: " << NvFbcResult(cr)
             << L" | sdk=0x" << std::hex << sdkVersion
             << L" | statusBefore=" << NvFbcResult(statusBeforeResult)
             << L" flagsBefore=0x" << std::hex << statusBefore.flags
             << L" captureBitBefore=" << (captureBefore ? 1 : 0)
             << L" | enable=" << NvFbcResult(enableResult)
             << L" | statusAfter=" << NvFbcResult(sr)
             << L" flagsAfter=0x" << std::hex << status.flags
             << L" captureBitAfter=" << (captureAfter ? 1 : 0)
             << L" | adapter=" << std::dec << dxgiAdapterIndex;
        error = diag.str();
        Shutdown(); return false;
'@
if (-not $text.Contains($old)) { throw 'V35.1 CreateEx diagnostic patch target was not found.' }
$text = $text.Replace($old, $new)

[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
Write-Host 'Applied V35.1 NvFBC elevation/status diagnostic patch.'
