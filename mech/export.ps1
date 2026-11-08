# Export STL files for printing.
#   .\export.ps1                      -> all parts and fit tests
#   .\export.ps1 -Parts fit_tp_width   -> one part
# Output goes to mech\stl\

param(
  [string[]]$Parts = @("fit_tp_width", "fit_tp_thick", "fit_screw_gauge", "fit_hub_slot",
                       "base", "hub", "out_gear", "pinion", "bridge", "lid", "knob")
)

# openscad.com is the console build on Windows: it waits, prints errors and sets the exit code.
$scad = (Get-Command openscad.com -ErrorAction SilentlyContinue).Source
if (-not $scad) { $scad = "C:\Program Files\OpenSCAD\openscad.com" }
if (-not (Test-Path $scad)) { throw "OpenSCAD not found. Install it with: winget install OpenSCAD.OpenSCAD" }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "stl"
New-Item -ItemType Directory -Force $out | Out-Null

$failed = @()
foreach ($p in $Parts) {
  $dst = Join-Path $out "$p.stl"
  Write-Host "Exporting $p -> $dst"
  # Backslash-escaped quotes so OpenSCAD receives part="name" as a string.
  & $scad -o $dst -D "part=\`"$p\`"" (Join-Path $here "print.scad")
  if ($LASTEXITCODE -ne 0) { $failed += $p }
}
if ($failed.Count -gt 0) { throw "Export failed for: $($failed -join ', ')" }
Write-Host "Done. STL files are in $out"
