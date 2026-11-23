# SS2 Steam Invite Fix - installer.
#   Install:   irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1 | iex
#   Uninstall: & ([scriptblock]::Create((irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1))) -Uninstall
# Optional: -GamePath "D:\...\Serious Sam 2"
param([string]$GamePath, [switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$Repo = 'duqisuki/ss2-steam-invite-fix'
$DllUrl = "https://raw.githubusercontent.com/$Repo/main/dist/steam_api.dll"
$Marker = 'ss2_invite_fix.log'   # string that only exists inside our proxy DLL

function Say($msg, $color = 'Gray') { Write-Host "[SS2 fix] $msg" -ForegroundColor $color }

function Find-GamePath {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { $steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath }
    if (-not $steam) { return $null }
    $libs = @($steam -replace '/', '\')
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $acf = Join-Path $lib 'steamapps\appmanifest_204340.acf'
        if (Test-Path $acf) {
            $dir = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"').Groups[1].Value
            if (-not $dir) { $dir = 'Serious Sam 2' }
            $p = Join-Path $lib "steamapps\common\$dir"
            if (Test-Path (Join-Path $p 'Bin\Sam2.exe')) { return $p }
        }
    }
    return $null
}

function Test-IsProxy($file) {
    if (-not (Test-Path $file)) { return $false }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file)).Contains($Marker)
}

if (-not $GamePath) { $GamePath = Find-GamePath }
if (-not $GamePath -or -not (Test-Path (Join-Path $GamePath 'Bin\Sam2.exe'))) {
    Say 'Serious Sam 2 not found. Run again with -GamePath "<game folder>".' Red
    return
}
$bin  = Join-Path $GamePath 'Bin'
$api  = Join-Path $bin 'steam_api.dll'
$orig = Join-Path $bin 'steam_api_orig.dll'
Say "Game: $GamePath"

if (Get-Process Sam2, DedicatedServer -ErrorAction SilentlyContinue) {
    Say 'Close Serious Sam 2 first, then run the command again.' Red
    return
}

if ($Uninstall) {
    if ((Test-IsProxy $api) -and (Test-Path $orig)) {
        Move-Item $orig $api -Force
        Remove-Item (Join-Path $bin $Marker) -ErrorAction SilentlyContinue
        Say 'Removed. Original steam_api.dll restored.' Green
    } else {
        Say 'Fix is not installed.' Yellow
    }
    return
}

# Get the proxy DLL: local copy next to this script, or download from GitHub.
$tmp = Join-Path $env:TEMP 'ss2_invite_fix_steam_api.dll'
$local = if ($PSScriptRoot) { Join-Path $PSScriptRoot 'dist\steam_api.dll' } else { $null }
if ($local -and (Test-Path $local)) {
    Copy-Item $local $tmp -Force
} else {
    Say 'Downloading...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest $DllUrl -OutFile $tmp -UseBasicParsing
}
if (-not (Test-IsProxy $tmp)) { throw 'Downloaded file is not the SS2 invite fix DLL.' }

# Keep the real Steam DLL as steam_api_orig.dll. If the current steam_api.dll is
# not ours (fresh install or Steam updated/verified the game), it is the original.
if (-not (Test-IsProxy $api)) {
    Copy-Item $api $orig -Force
} elseif (-not (Test-Path $orig)) {
    throw 'steam_api_orig.dll is missing. Verify game files in Steam, then run this again.'
}
Copy-Item $tmp $api -Force
Remove-Item $tmp -ErrorAction SilentlyContinue

Say 'Installed! Accepting a Steam invite now joins the host''s game.' Green
Say 'If Steam updates or verifies the game, run this command again.' Gray
