<#
    The fresh-install check: Doom VR the way a new player meets it. No
    settings file at all, SteamVR (its null driver, no headset needed), Doom II
    first, then Switch Game in the same session to Heretic and then Hexen. Every
    game must come up with its VR buttons: the menu button on the menu, A on
    use. Run it before every test is staged and before every release.

        powershell -ExecutionPolicy Bypass -File tools\fresh-install-check.ps1

    Why: until 25 Nov 2026 every test ran on the owner's own settings, copied
    forward since September, so a game with no saved buttons never came up -
    and a new player's first switch into Heretic had no menu and no use button
    (Flat2VR report). Exit 0 = PASS, 1 = FAIL.
#>
param(
    [string[]]$Chain = @("Heretic", "Hexen"),
    [int]$Timeout = 240
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo "build-vr\Release"
$iwad = Join-Path $bin "wads\DOOM2.WAD"
foreach ($g in $Chain) { if (-not (Test-Path (Join-Path $bin "launchers\$g.bat"))) { throw "no launcher for $g in $bin\launchers" } }

# vr-headless.ps1 starts each run in a new empty folder: its settings file
# does not exist yet, as on a new player's PC.
$env:DOOMVR_TEST_CHAIN = ($Chain -join "|")
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "vr-headless.ps1") `
        -Iwad $iwad -Steps "echo fresh-install check" -FollowSuccessor -Timeout $Timeout | Out-Host
} finally {
    Remove-Item Env:\DOOMVR_TEST_CHAIN -ErrorAction SilentlyContinue
}

$log = Join-Path $bin "log-doomvr-headless-vr.txt"
if (-not (Test-Path $log)) { $log = Get-ChildItem $bin -Filter "log-doomvr-headless*.txt" | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName }
if (-not $log) { Write-Output "FAIL: no game log"; exit 1 }
$lines = Get-Content -LiteralPath $log
$games = @("Doom II") + $Chain
$found = @($lines | Where-Object { $_ -match "DoomVR: VR buttons for this game: menu = (.*), A = (.*), Frame menu = (.*)$" })
$fail = $false
Write-Output ""
Write-Output "Fresh-install check ($log)"
for ($i = 0; $i -lt $games.Count; ++$i) {
    if ($i -ge $found.Count) { Write-Output ("  {0,-8} FAIL  never reported its VR buttons" -f $games[$i]); $fail = $true; continue }
    $null = $found[$i] -match "menu = (.*), A = (.*), Frame menu = (.*)$"
    $menu = $Matches[1]; $use = $Matches[2]; $fmenu = $Matches[3]
    $ok = $menu -eq "menu_main" -and $use -eq "+use" -and $fmenu -eq "menu_main"
    if (-not $ok) { $fail = $true }
    Write-Output ("  {0,-8} {1}  menu = {2}, A = {3}, Frame menu = {4}" -f $games[$i], $(if ($ok) { "PASS" } else { "FAIL" }), $menu, $use, $fmenu)
}
if ($fail) { Write-Output "FAIL"; exit 1 }
Write-Output "PASS"
exit 0
