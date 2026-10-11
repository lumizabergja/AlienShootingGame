$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dst = Join-Path $root 'third_party/streamline'
$zip = Join-Path $env:TEMP 'streamline-sdk-v2.14.1.zip'
$url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip'
$expected = '92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b'
if(Test-Path $dst){ Remove-Item $dst -Recurse -Force }
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
$actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
if($actual -ne $expected){ throw "Streamline SHA-256 mismatch: $actual" }
$tmp = Join-Path $env:TEMP ('sl2141_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
Expand-Archive -Path $zip -DestinationPath $tmp -Force
$candidates = @()
if(Test-Path (Join-Path $tmp 'include/sl.h')) { $candidates += (Get-Item $tmp) }
$candidates += @(Get-ChildItem $tmp -Directory -Recurse | Where-Object { Test-Path (Join-Path $_.FullName 'include/sl.h') })
if(-not $candidates -or $candidates.Count -eq 0){
  Get-ChildItem $tmp -Force | ForEach-Object { Write-Host ("SDK root entry: " + $_.FullName) }
  throw 'Unable to locate Streamline SDK root after extraction.'
}
$src = $candidates[0].FullName
New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
Copy-Item $src $dst -Recurse -Force
Write-Host "Streamline 2.14.1 verified: $actual"
