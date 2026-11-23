# Builds dist\steam_api.dll (32-bit) with Zig's C compiler.
# Usage: .\build.ps1 [-Zig C:\path\to\zig.exe]
param([string]$Zig = 'zig')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = "$root\dist\steam_api.dll"
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
& $Zig cc -target x86-windows-gnu -shared -O2 -s `
    -o $out "$root\src\proxy.c" "$root\src\steam_api.def" `
    -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item "$root\dist\*.lib", "$root\dist\*.pdb" -ErrorAction SilentlyContinue

# The i386 MinGW linker decorates forwarder targets as "_steam_api_orig.Name".
# Shift each string left by one byte in place, so it reads "steam_api_orig.Name".
$bytes = [IO.File]::ReadAllBytes($out)
$enc = [Text.Encoding]::ASCII
$needle = $enc.GetBytes('_steam_api_orig.')
$fixed = 0
for ($i = 0; $i -le $bytes.Length - $needle.Length; $i++) {
    $match = $true
    for ($j = 0; $j -lt $needle.Length; $j++) { if ($bytes[$i + $j] -ne $needle[$j]) { $match = $false; break } }
    if (-not $match) { continue }
    $k = $i
    while ($bytes[$k + 1] -ne 0) { $bytes[$k] = $bytes[$k + 1]; $k++ }
    $bytes[$k] = 0
    $fixed++
}
[IO.File]::WriteAllBytes($out, $bytes)
"Forwarders fixed: $fixed"
Get-Item $out | Select-Object Name, Length
