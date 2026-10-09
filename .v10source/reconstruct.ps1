$ErrorActionPreference = 'Stop'
$chunks = Get-ChildItem "$env:GITHUB_WORKSPACE\.v5payload" -Filter '*.txt' | Sort-Object Name
if ($chunks.Count -ne 8) { throw "Expected 8 V5 chunks, found $($chunks.Count)" }
$b64 = ($chunks | ForEach-Object { Get-Content $_.FullName -Raw }) -join ''
$zip = "$env:GITHUB_WORKSPACE\v5base.zip"
[IO.File]::WriteAllBytes($zip, [Convert]::FromBase64String($b64))
Expand-Archive $zip "$env:GITHUB_WORKSPACE\v10work" -Force
$src = "$env:GITHUB_WORKSPACE\v10work\source"

$dir = "$env:GITHUB_WORKSPACE\.v6patch2"
$first = Get-Content "$dir\00.txt" -Raw
$pieces = @($first.Substring(0, 6000))
foreach ($i in 1..6) { $pieces += (Get-Content (Join-Path $dir ("{0:D2}.txt" -f $i)) -Raw) }
$p6 = "$env:GITHUB_WORKSPACE\v6.patch"
[IO.File]::WriteAllBytes($p6, [Convert]::FromBase64String(($pieces -join '')))
if ((Get-FileHash $p6 -Algorithm SHA256).Hash.ToLowerInvariant() -ne '11d27d692845d72048314760b6e92e9f34094e1c5e019fca0e7079427a556cc1') { throw 'V6 SHA mismatch' }
git apply --directory=v10work/source $p6
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$p6ui = "$env:GITHUB_WORKSPACE\v6-ui.patch"
[IO.File]::WriteAllBytes($p6ui, [Convert]::FromBase64String((Get-Content "$dir\ui-cleanup.txt" -Raw)))
if ((Get-FileHash $p6ui -Algorithm SHA256).Hash.ToLowerInvariant() -ne '7b01691db1b612944a5062327a489ec9dc4af45af4f89a9a487dee3d7bb1a713') { throw 'V6 UI SHA mismatch' }
git apply --directory=v10work/source $p6ui
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$v7parts = @('00.txt','01.txt','02.txt') | ForEach-Object { Get-Content (Join-Path "$env:GITHUB_WORKSPACE\.v7gz" $_) -Raw }
$v7gz = "$env:GITHUB_WORKSPACE\v7.gz"
[IO.File]::WriteAllBytes($v7gz, [Convert]::FromBase64String(($v7parts -join '')))
$p7 = "$env:GITHUB_WORKSPACE\v7.patch"
$input=[IO.File]::OpenRead($v7gz)
try {
  $gzip=[IO.Compression.GzipStream]::new($input,[IO.Compression.CompressionMode]::Decompress)
  try { $out=[IO.File]::Create($p7); try { $gzip.CopyTo($out) } finally { $out.Dispose() } }
  finally { $gzip.Dispose() }
} finally { $input.Dispose() }
if ((Get-FileHash $p7 -Algorithm SHA256).Hash.ToLowerInvariant() -ne '94a9009b12d6e702511574733c9ea9148cfbc733a37291d891d477766d22ed8a') { throw 'V7 SHA mismatch' }
git apply --directory=v10work/source $p7
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$encoded = Get-Content "$env:GITHUB_WORKSPACE\.v8patch\v8patch.gz.b64" -Raw
$v8gz = "$env:GITHUB_WORKSPACE\v8.patch.gz"
[IO.File]::WriteAllBytes($v8gz, [Convert]::FromBase64String($encoded))
$p8 = "$env:GITHUB_WORKSPACE\v8.patch"
$input=[IO.File]::OpenRead($v8gz)
try {
  $gzip=[IO.Compression.GzipStream]::new($input,[IO.Compression.CompressionMode]::Decompress)
  try { $out=[IO.File]::Create($p8); try { $gzip.CopyTo($out) } finally { $out.Dispose() } }
  finally { $gzip.Dispose() }
} finally { $input.Dispose() }
if ((Get-FileHash $p8 -Algorithm SHA256).Hash.ToLowerInvariant() -ne '4a127cde81c1485051db5392813701887a3c794b6269aa6019e3a83991dbd6ff') { throw 'V8 SHA mismatch' }
git apply --directory=v10work/source $p8
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$raw9 = @('00.patchpart','01.patchpart','02.patchpart','03.patchpart') | ForEach-Object { Get-Content (Join-Path "$env:GITHUB_WORKSPACE\.v9raw" $_) -Raw }
$p9 = "$env:GITHUB_WORKSPACE\v9.patch"
[IO.File]::WriteAllText($p9, ($raw9 -join ''), [Text.UTF8Encoding]::new($false))
$sha9=(Get-FileHash $p9 -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha9 -ne '1fbebdc7819ab4a18e7cd97b76fdec08a5c6e4f9775df27f1e345c747dbeb8e7') { throw "V9 SHA mismatch: $sha9" }
git apply --check --directory=v10work/source $p9
if ($LASTEXITCODE) { exit $LASTEXITCODE }
git apply --directory=v10work/source $p9
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Verify and copy exact V10 runtime files.
$rts = "$env:GITHUB_WORKSPACE\.v10source\runtime.cpp"
$rth = "$env:GITHUB_WORKSPACE\.v10source\runtime.h"
if ((Get-FileHash $rts -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd66f938d56767eb01d1c388a997dfd41007acffc4eeb20286cede10fbd693fe0') { throw 'V10 runtime.cpp SHA mismatch' }
if ((Get-FileHash $rth -Algorithm SHA256).Hash.ToLowerInvariant() -ne '44add56c6483437cba84b752365d11720d3567e6d7e0985b877524dff8c4517e') { throw 'V10 runtime.h SHA mismatch' }
Copy-Item $rts "$src\src\runtime.cpp" -Force
Copy-Item $rth "$src\src\runtime.h" -Force

function Replace-Exact([string]$path, [string]$old, [string]$new) {
  $s = Get-Content $path -Raw
  if (-not $s.Contains($old)) { throw "Expected text not found in $path : $old" }
  $s = $s.Replace($old, $new)
  [IO.File]::WriteAllText($path, $s, [Text.UTF8Encoding]::new($false))
}

Replace-Exact "$src\src\engine.cpp" 'V9 CHI BOOTSTRAP:' 'V10 CHI BOOTSTRAP:'
Replace-Exact "$src\src\engine.cpp" 'V9 CHI FINAL-PRESENT WARP ACTIVE:' 'V10 CHI FINAL-PRESENT WARP ACTIVE:'
Replace-Exact "$src\src\main.cpp" 'V9 CHI FINAL-PRESENT LATE WARP' 'V10 CHI FINAL-PRESENT LATE WARP'
Replace-Exact "$src\src\post_present.cpp" 'v9.chi-probe' 'v10.chi-probe'
Replace-Exact "$src\src\post_present.cpp" 'V9 CHI final-present late warp.' 'V10 CHI final-present late warp.'
Replace-Exact "$src\src\post_present.h" '// V9:' '// V10:'

$rt=Get-Content "$src\src\runtime.cpp" -Raw
$rh=Get-Content "$src\src\runtime.h" -Raw
$pp=Get-Content "$src\src\post_present.cpp" -Raw
if (-not $rt.Contains('discoverCommonComputeSlot')) { throw 'V10 compute-slot scanner missing' }
if (-not $rt.Contains('sl.common direct compute slot resolved:')) { throw 'V10 slot diagnostic missing' }
if (-not $rt.Contains('compute pointer=')) { throw 'V10 pointer diagnostic missing' }
if ($rt.Contains('hookParamSetPointer') -or $rt.Contains('parameter set(void*) calls=') -or $rt.Contains('slOnPluginLoad wrapper calls=')) { throw 'Stale V8/V9 parameter hook survived' }
if (-not $rh.Contains('commonComputeSlot')) { throw 'V10 runtime header missing compute slot' }
if (-not $pp.Contains('v10.chi-probe')) { throw 'V10 CHI probe label missing' }
Write-Host 'Exact V9 source reconstructed; exact V10 no-parameter-hook runtime installed.'
