$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$windres = 'C:\msys64\ucrt64\bin\windres.exe'
$gxx = 'C:\msys64\ucrt64\bin\x86_64-w64-mingw32-g++.exe'

if (-not (Test-Path $windres)) { throw "Missing $windres" }
if (-not (Test-Path $gxx)) { throw "Missing $gxx" }

& $windres (Join-Path $root 'msinfo.rc') -O coff -o (Join-Path $out 'msinfo.res.o')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $gxx `
  -std=c++20 -O2 -s -municode -mwindows `
  (Join-Path $root 'src\main.cpp') `
  (Join-Path $root 'src\wmi.cpp') `
  (Join-Path $root 'src\categories.cpp') `
  (Join-Path $out 'msinfo.res.o') `
  -lole32 -loleaut32 -lwbemuuid -lcomctl32 -lcomdlg32 -lshell32 `
  -o (Join-Path $out 'msinfo32_recovered.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built: $(Join-Path $out 'msinfo32_recovered.exe')"

