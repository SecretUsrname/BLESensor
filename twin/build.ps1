# Build slock_twin.dll: the real firmware core + simulated mechanism, for the Python twin host.
# Needs a C++17 g++ on PATH (MSYS2 ucrt64). Statically linked so Python can load it without
# the MSYS2 runtime DLLs.

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$fw = Join-Path (Split-Path -Parent $here) "firmware"
$build = Join-Path $here "build"
New-Item -ItemType Directory -Force $build | Out-Null

$sources = @(Get-ChildItem (Join-Path $fw "lib\slock_core\src\slock\*.cpp")) +
           @(Get-Item (Join-Path $here "core\twin_api.cpp")) | ForEach-Object { $_.FullName }

& g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -Wall -Wextra -Werror `
      -I (Join-Path $fw "lib\slock_core\src") -I (Join-Path $fw "test_host") `
      @sources -o (Join-Path $build "slock_twin.dll")
if ($LASTEXITCODE -ne 0) { Write-Host "DLL build failed"; exit 1 }
Write-Host "Built $build\slock_twin.dll"
