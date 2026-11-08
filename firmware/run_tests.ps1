# Build and run the host unit tests for the portable core (lib/slock_core) with g++.
# Needs a C++17 g++ on PATH (e.g. MSYS2 ucrt64). No PlatformIO or hardware required.
#   .\firmware\run_tests.ps1

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"
New-Item -ItemType Directory -Force $build | Out-Null

$sources = @(Get-ChildItem (Join-Path $here "lib\slock_core\src\slock\*.cpp")) +
           @(Get-ChildItem (Join-Path $here "test_host\*.cpp")) | ForEach-Object { $_.FullName }
$exe = Join-Path $build "host_tests.exe"

& g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion `
      -I (Join-Path $here "lib\slock_core\src") -I (Join-Path $here "test_host") `
      @sources -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed"; exit 1 }

& $exe
exit $LASTEXITCODE
