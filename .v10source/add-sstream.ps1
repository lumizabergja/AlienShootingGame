$ErrorActionPreference = 'Stop'
$path = "$env:GITHUB_WORKSPACE\v10work\source\src\runtime.cpp"
$s = Get-Content $path -Raw
if ($s.Contains('#include <sstream>')) { throw 'runtime.cpp already contains <sstream> before the V10 compile fix' }
$needle = "#include <wintrust.h>`n"
if (-not $s.Contains($needle)) { throw 'Expected wintrust include was not found in V10 runtime.cpp' }
$s = $s.Replace($needle, "#include <wintrust.h>`n#include <sstream>`n")
[IO.File]::WriteAllText($path, $s, [Text.UTF8Encoding]::new($false))
$sha = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha -ne '69d6b0671bd1ce8fd492cb28f634bd5f815202f9f82cf8d0b9bc935d73465304') { throw "V10 runtime.cpp post-include SHA mismatch: $sha" }
Write-Host "V10 compiler include fix applied exactly. SHA256=$sha"
