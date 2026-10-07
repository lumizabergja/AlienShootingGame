$ErrorActionPreference = 'Stop'
$path = 'v35nvfbc/source/nvfbc_cuda_capture.cpp'
$text = Get-Content $path -Raw

$old = @'
    NvFBCCreateParams create{};
    create.version = NvFbcStructVersion(sizeof(create), 2);
    create.interfaceType = NVFBC_TO_DX9_VID;
    create.device = impl_->d3dDevice;
    create.adapterIdx = dxgiAdapterIndex;
    int cr = impl_->nvfbc.createEx(&create);
    if (cr != NVFBC_SUCCESS) {
        create = {};
        create.version = NvFbcStructVersion(sizeof(create), 1);
        create.interfaceType = NVFBC_TO_DX9_VID;
        create.device = impl_->d3dDevice;
        create.adapterIdx = dxgiAdapterIndex;
        cr = impl_->nvfbc.createEx(&create);
    }
'@
$new = @'
    NvFBCCreateParams create{};
    create.version = NvFbcStructVersion(sizeof(create), 2);
    create.interfaceType = NVFBC_TO_DX9_VID;
    create.maxDisplayWidth = ~0u;
    create.maxDisplayHeight = ~0u;
    create.device = impl_->d3dDevice;
    create.interfaceVersion = sdkVersion;
    create.adapterIdx = dxgiAdapterIndex;
    int cr = impl_->nvfbc.createEx(&create);
    if (cr != NVFBC_SUCCESS) {
        create = {};
        create.version = NvFbcStructVersion(sizeof(create), 1);
        create.interfaceType = NVFBC_TO_DX9_VID;
        create.maxDisplayWidth = ~0u;
        create.maxDisplayHeight = ~0u;
        create.device = impl_->d3dDevice;
        create.interfaceVersion = sdkVersion;
        create.adapterIdx = dxgiAdapterIndex;
        cr = impl_->nvfbc.createEx(&create);
    }
'@
if (-not $text.Contains($old)) { throw 'V35.3 CreateEx patch target not found.' }
$text = $text.Replace($old, $new)

$text = $text.Replace('NvFBC_CreateEx(NvFBCToDx9Vid) failed: ', 'NvFBC_CreateEx(NvFBCToDx9Vid, interfaceVersion=0x' + '" << std::hex << sdkVersion << L"' + ') failed: ')
$text = $text.Replace('V35.2 NvFBC DX9Vid build.', 'V35.3 NvFBC DX9Vid build.')

[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
Write-Host 'Applied V35.3 NvFBC interface-version CreateEx fix.'
