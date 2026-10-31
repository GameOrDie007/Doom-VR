<#
    DoomVR setup - run by SETUP.bat.

    Finds the Doom-family games the player owns and makes them playable:

      1. Looks for their game files in this install, in any folder dropped on
         SETUP.bat, in every Steam library, and in every GOG.com install.
      2. Copies the original games' files into wads\ (with the files some of
         them need beside them - voices.wad for Strife), and each re-release
         set whole into kex\: Doom + Doom II (2024) and Heretic + Hexen (2025).
      3. For Heretic + Hexen (2025), fetches trzejos's heretic-hexen-zdpatches
         from its GitHub release into mods\ - the re-release's new monsters,
         weapons and items are defined in a form UZDoom does not read, and
         that patch defines them for it. And it writes the remastered-music
         patches, from the re-release's own track list, into mods\.
      4. Fetches the regular mods - megawads, Brutal Doom, the voxel packs,
         total conversions - from where their authors publish them, into
         mods\ (or wads\ for a standalone game). Ones on sites that refuse
         scripts (ModDB) are named with their page; download them once and
         the next run picks them up from your Downloads folder.
      5. DOOM 64 (Nightdive's 2020 re-release) through DOOM 64 CE, and the
         PlayStation Doom games through PSX Doom CE (molecicco): CE's download
         is unpacked into doomce\, and the player's DOOM64.WAD is patched into
         the DOOM64.IWAD CE needs - by setup itself (bps.ps1), checked byte for
         byte, without running anyone's patching program.
      6. Writes a launcher per game and mod into launchers\ - what Switch
         Game lists, grouped - and DoomVR.bat.
      7. Makes the art: a cover for every game from that game's own title
         screen (art.ps1), and a splash from all of them - unless the player
         has put their own in boxart\ - packed into boxart.pk3, which the
         engine loads for every game.

    Re-runnable, and it only adds: a file already in wads\, kex\ or mods\ is
    never replaced (the generated music patches excepted - they are ours), a
    launcher is only written for a game whose files are all there, no launcher
    is deleted, and the player's settings (doomvr.ini) and saves are not
    touched. Nothing here is game data or anyone else's work: the player's own
    files are copied on the player's own machine, and the patch is fetched
    from where its author publishes it.

    Written for Doom VR from Steam's libraryfolders.vdf format, GOG.com's registry
    entries, UZDoom's own Steam and GOG folder knowledge (src/win32/i_steam.cpp),
    the WAD directory format, and Game Or Die's earlier setup scripts (ours:
    the re-release layouts, the tested load orders, and the TRAKINFO ->
    $musicalias music patch).

    For building test folders only (tools\write-launchers.ps1 calls this):
        setup.ps1 -Install <folder> -LinkWads <path relative to the install>
                  [-LinkWadsCheck <where that folder is on this machine>]
    writes launchers that read the IWADs where they are, copies nothing,
    fetches nothing and searches nowhere else.

    Copyright (C) 2026 Game Or Die. All rights reserved. May be
    redistributed only as part of Doom VR, unchanged.
#>
# No positional parameters: a folder dropped on SETUP.bat arrives as a bare
# argument, and would otherwise be taken as -Install.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Install = "",
    [switch]$NoSteam,
    [switch]$NoGog,
    [switch]$NoDownload,
    # No questions and no waiting for downloads (tests, and runs with no one
    # at the keyboard).
    [switch]$Unattended,
    # Only the Modern VR look's own downloads (Voxel Doom, NashGore and its
    # voxel gore, the upscaled textures): asked about and waited for, the
    # other one-click mods only listed. "Get the Modern VR look.cmd" runs this.
    [switch]$LookOnly,
    # Where the mirrored mods come from, in place of Doom VR's own "mods"
    # release: a test build points this at a local copy (file:///...).
    [string]$Mirror = "",
    # Tests of the guided download: answer "yes" without a keyboard, open no
    # browser, and stop waiting after this many seconds.
    [switch]$GuidedTest,
    [int]$GuidedTestSeconds = 30,
    # Where manually downloaded mods are looked for.
    [string]$Downloads = (Join-Path $env:USERPROFILE "Downloads"),
    [string]$SteamRoot = "",
    [string]$LinkWads = "",
    [string]$LinkWadsCheck = "",
    # Where the Heretic + Hexen patch is fetched from (a test can point it at
    # a local folder with file:///).
    [string]$PatchBase = "https://github.com/trzejos/heretic-hexen-zdpatches/releases/download/v0.2.0",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Search = @()
)

$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 redraws a progress bar for every block a download
# reads: with it on, a 67 MB file takes half a minute instead of half a second.
$ProgressPreference = "SilentlyContinue"
$setupClock = [Diagnostics.Stopwatch]::StartNew()
if (-not $Install) { $Install = Split-Path -Parent $PSScriptRoot }

# The log is written line by line from the first moment, and whatever stops
# the run is written to it too: a set-up that fails part-way on a player's
# machine must leave its reason behind, not an empty folder.
$logPath = Join-Path $Install "log-setup.txt"
function Say([string]$line) {
    Write-Host $line
    # The log gets the minutes and seconds since the start, to see what is slow.
    $at = "[{0:00}:{1:00}] " -f [math]::Floor($setupClock.Elapsed.TotalMinutes), $setupClock.Elapsed.Seconds
    try { [System.IO.File]::AppendAllText($logPath, $at + $line + "`r`n") } catch { }
}
try { [System.IO.File]::WriteAllText($logPath, "") } catch { }
trap {
    Say ("SETUP STOPPED: " + $_.Exception.Message)
    Say ("  at " + $_.InvocationInfo.PositionMessage.Split("`n")[0])
    break
}
Say ("Doom VR setup started " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " from " + $PSScriptRoot)

# ProviderPath, not Path: run from a network folder, .Path is
# "Microsoft.PowerShell.Core\FileSystem::\\server\...", which .NET cannot write to.
$Install = (Resolve-Path -LiteralPath $Install).ProviderPath
if (-not (Test-Path (Join-Path $Install "uzdoom.exe"))) { throw "no uzdoom.exe in $Install - run Setup.bat from the Doom VR download" }

# ---------------------------------------------------------------------------
# The games. Each file is named by the folder it lives in: wads\ for the
# original games and free standalone ones, kex\ for the re-releases, mods\
# for what setup fetches or writes. A launcher is written when every file is
# there. Several entries may share a name: the first whose files are all
# there is the one written.
#
# The load orders are the ones tested on an earlier UZDoom 5 build of ours and
# again on DoomVR. The re-release's id24res.wad and extras.wad load by
# themselves from beside its IWAD; nerve.wad does not, so it is named.
# ---------------------------------------------------------------------------
$hhCommon = "mods\hh_patch_common.pk3"
$games = @(
    @{ name = "Doom";                              iwad = "wads\DOOM.WAD" },
    @{ name = "Doom (2024 re-release)";            iwad = "kex\doom.wad" },
    @{ name = "Doom - SIGIL";                      iwad = "kex\doom.wad";  files = @("kex\sigil.wad") },
    @{ name = "Doom - SIGIL II";                   iwad = "kex\doom.wad";  files = @("kex\sigil2.wad") },
    @{ name = "Doom II";                           iwad = "wads\DOOM2.WAD" },
    @{ name = "Doom II (2024 re-release)";         iwad = "kex\doom2.wad" },
    @{ name = "Doom II - No Rest for the Living";  iwad = "kex\doom2.wad"; files = @("kex\nerve.wad") },
    @{ name = "Doom II - No Rest for the Living";  iwad = "wads\DOOM2.WAD"; files = @("wads\NERVE.WAD") },
    @{ name = "Doom II - Master Levels";           iwad = "kex\doom2.wad"; files = @("kex\masterlevels.wad") },
    # Legacy of Rust is five wads: the maps and four companions. UZDoom 5's
    # ID24 support is what makes its new monsters spawn.
    @{ name = "Doom II - Legacy of Rust";          iwad = "kex\doom2.wad"; files = @("kex\id1.wad", "kex\id1-res.wad", "kex\id1-tex.wad", "kex\id1-weap.wad", "kex\id1-mus.wad") },
    @{ name = "Final Doom - TNT";                  iwad = "wads\TNT.WAD" },
    @{ name = "Final Doom - TNT";                  iwad = "kex\tnt.wad" },
    @{ name = "Final Doom - Plutonia";             iwad = "wads\PLUTONIA.WAD" },
    @{ name = "Final Doom - Plutonia";             iwad = "kex\plutonia.wad" },
    @{ name = "Heretic";                           iwad = "wads\HERETIC.WAD" },
    @{ name = "Heretic (2025 enhanced)";           iwad = "kex\heretic.wad"; files = @($hhCommon, "kex\heretic_ex.wad", "mods\hh_patch_heretic.pk3"); music = "heretic" },
    @{ name = "Heretic - Faith Renewed";           iwad = "kex\heretic.wad"; files = @($hhCommon, "kex\heretic_fr.wad", "mods\hh_patch_fr.pk3"); music = "heretic" },
    @{ name = "Hexen";                             iwad = "wads\HEXEN.WAD" },
    @{ name = "Hexen - Deathkings";                iwad = "wads\HEXDD.WAD"; needs = @("wads\HEXEN.WAD") },
    @{ name = "Hexen (2025 enhanced)";             iwad = "kex\hexen.wad"; files = @($hhCommon, "kex\hexen_ex.wad", "mods\hh_patch_hexen.pk3"); music = "hexen" },
    @{ name = "Hexen - Deathkings (2025)";         iwad = "kex\hexen.wad"; files = @($hhCommon, "kex\hexdd.wad", "kex\hexdd_ex.wad", "mods\hh_patch_hexdd.pk3"); music = "hexen" },
    @{ name = "Hexen - Vestiges of Grandeur";      iwad = "kex\hexen.wad"; files = @($hhCommon, "kex\hexen_vog.wad", "mods\hh_patch_vog.pk3"); music = "hexen" },
    @{ name = "Strife";                            iwad = "wads\strife1.wad" },
    @{ name = "Strife - Veteran Edition";          iwad = "wads\SVE.wad" },
    @{ name = "Chex Quest 3";                      iwad = "wads\chex3.wad" },
    @{ name = "HacX";                              iwad = "wads\HACX.WAD" },
    @{ name = "Freedoom Phase 1";                  iwad = "wads\freedoom1.wad" },
    @{ name = "Freedoom Phase 2";                  iwad = "wads\freedoom2.wad" },
    @{ name = "FreeDM";                            iwad = "wads\freedm.wad" },
    @{ name = "Blasphemer";                        iwad = "wads\blasphem.wad" },
    @{ name = "Adventures of Square";              iwad = "wads\square1.pk3" },
    @{ name = "Project Osiris";                    iwad = "wads\osiris.ipk3" },
    # rott_tc_full.pk3 on top of Doom II. Loaded on its own as an IWAD it
    # reaches its first map but lacks Doom's menu pictures (M_EPISOD, M_NEWG,
    # M_SKILL - script errors in the menus); version 1.6 is the standalone
    # one, for when the game library is done.
    @{ name = "Return of the Triad";               iwad = "wads\DOOM2.WAD"; files = @("wads\rott_tc_full.pk3") },
    @{ name = "Return of the Triad";               iwad = "kex\doom2.wad"; files = @("wads\rott_tc_full.pk3") },
    # DOOM CE (molecicco), built for UZDoom 5. Each ipk3 names what it needs
    # in its IWADINFO ("Required") and loads its addons from its own folder;
    # -iwaddir tells the engine where the required IWAD is.
    # -shaderlog for now: Doom 64 froze once on an RTX 5080 inside the
    # startup shader compile (CE's material shaders) and loaded fine the next
    # time; if it happens again the log names the shader. Take it out once
    # that is settled.
    @{ name = "Doom 64";                           iwad = "doomce\DOOM64.CE.ipk3";       needs = @("doomce\DOOM64.IWAD"); dirs = @("doomce"); args = "-shaderlog" },
    @{ name = "PSX Doom";                          iwad = "doomce\PSXDOOM.CE.ipk3";      needs = @("wads\DOOM2.WAD");      dirs = @("wads");   args = "-shaderlog" },
    @{ name = "PSX Final Doom";                    iwad = "doomce\PSXFINALDOOM.CE.ipk3"; needs = @("wads\DOOM2.WAD");      dirs = @("wads");   args = "-shaderlog" }
)

# The remastered soundtracks: the re-release's music wad, and the name map
# setup writes for it. Both load last, in the Modern VR style.
$musicSets = @{
    heretic = @{ iwad = "kex\heretic.wad"; wad = "kex\heretic_mus_remix.wad"; pk3 = "mods\music_heretic_remix.pk3" }
    hexen   = @{ iwad = "kex\hexen.wad";   wad = "kex\hexen_mus_remix.wad";   pk3 = "mods\music_hexen_remix.pk3" }
}

# ---------------------------------------------------------------------------
# The regular mods, from our earlier setup's catalogue, pinned to the
# releases tested there. VR-only mods (VR weapon packs, laser sight,
# flashlight, vest, the "-vr" conversions) are not fetched: they are their
# authors' to distribute. Every downloaded file must be a zip ("PK") -
# a web page saved under a .pk3 name is the failure this catches.
#
#   kind = url      a direct link a script may fetch
#   kind = idgames  the Doomworld archive, resolved through its API
#   kind = manual   behind a browser check (ModDB); collected from Downloads
# dest   = "mods" (default) or "wads" for a standalone game
# unzip  = one name, or a list of wildcards, to lift out of the archive
# match  = for manual entries, what the download is called
# ---------------------------------------------------------------------------
# The one-click mods, mirrored on Doom VR's own GitHub repo as a pre-release
# named "mods" (unmodified, credited, each removed if its author asks): fetched
# from there so no player clicks through ModDB, each checked against its
# fingerprint. A file the mirror no longer has falls back to its author's
# page, as a one-click download.
$mirror = if ($Mirror -and (Test-Path -LiteralPath $Mirror -PathType Container)) { ([Uri]((Resolve-Path -LiteralPath $Mirror).ProviderPath)).AbsoluteUri.TrimEnd('/') } elseif ($Mirror) { $Mirror.TrimEnd('/') } else { "https://github.com/GameOrDie007/Doom-VR/releases/download/mods-1" }
$catalogue = @(
    # --- free standalone games -----------------------------------------------
    @{ name = "freedoom1.wad"; kind = "url"; dest = "wads"; url = "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip"; unzip = "freedoom1.wad"; what = "Freedoom Phase 1 - a free Doom" },
    @{ name = "freedoom2.wad"; kind = "url"; dest = "wads"; url = "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip"; unzip = "freedoom2.wad"; what = "Freedoom Phase 2" },
    @{ name = "freedm.wad";    kind = "url"; dest = "wads"; url = "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedm-0.13.0.zip";    unzip = "freedm.wad";    what = "FreeDM" },
    @{ name = "blasphem.wad";  kind = "url"; dest = "wads"; url = "https://github.com/Blasphemer/blasphemer/releases/download/v0.1.8/blasphem-0.1.8.zip"; unzip = "blasphem.wad"; what = "Blasphemer - a free Heretic" },
    @{ name = "chex3.wad";  dest = "wads"; kind = "url"; url = "$mirror/chex-quest-3.zip"; sha256 = "b71e6a5de70890ae0c3d1d1005a8fa4c6c1f639b6292d578f9f16ef85f02ea11"; match = "chex*3*"; unzip = "chex3.wad"; page = "https://www.chexquest3.com/";                   what = "Chex Quest 3 - free" },
    @{ name = "hacx.wad";   dest = "wads"; kind = "url"; url = "$mirror/hacx-2.0.zip"; sha256 = "9c58db264b59833b727149b7fed870b5170c8cc3e49db1c4396d1b7ee6930e6d"; match = "hacx*";   unzip = "hacx.wad"; page = "https://www.moddb.com/mods/hacx/downloads";     what = "HacX 2.0 - free" },
    @{ name = "Castlevania.ipk3"; dest = "wads"; kind = "url"; url = "$mirror/castlevania-simons-destiny.zip"; sha256 = "cfe7c1d7cafb002e2c450c6f804d73dad5b2648b33813e92503879b9e4b8a661"; match = "Castlevania*"; unzip = "Castlevania.ipk3"; page = "https://www.moddb.com/mods/castlevania-simons-destiny"; what = "Castlevania: Simon's Destiny - standalone" },
    @{ name = "osiris.ipk3";      dest = "wads"; kind = "url"; url = "$mirror/project-osiris.zip"; sha256 = "231688b5a7c516dce0eec5d386fd3d85118dbadcdae5859c10d116a820561e5f"; match = "osiris*";      unzip = "osiris.ipk3"; page = "https://www.moddb.com/mods/project-osiris";            what = "Project Osiris (Alien Breed 3D) - standalone" },
    @{ name = "rott_tc_full.pk3"; dest = "wads"; kind = "url"; url = "$mirror/return-of-the-triad.zip"; sha256 = "b35191c5ce9f6b1be16ab025049c1baf950fa5908d99f6e23f2d48fefd9daf6d"; match = "rott_tc*";     unzip = "rott_tc_full.pk3"; page = "https://www.moddb.com/mods/return-of-the-triad";       what = "Return of the Triad" },
    @{ name = "square1.pk3";      dest = "wads"; kind = "url"; url = "$mirror/adventures-of-square-1.zip"; sha256 = "de5cfbd88eeb7d708b01d986e1d7fc19ad25f192ee630ad8e54f2f27c422485e"; match = "square*";      unzip = "square1.pk3"; page = "https://adventuresofsquare.com/download.php";          what = "The Adventures of Square - free" },

    # --- DOOM CE (molecicco, ModDB): Doom 64, PSX Doom, PSX Final Doom --------
    # Lite: the three games. Full (Doom 64 only): Doom 64 with its addons -
    # extended music, HQ sounds, PBR materials, brightmaps. Either gives Doom
    # 64; both can be there. The patch for DOOM64.WAD comes out too.
    @{ name = "PSXDOOM.CE.ipk3"; dest = "doomce"; kind = "url"; url = "$mirror/doom-ce-4.0-lite.zip"; sha256 = "5d575a2fa0e3641cb905814fb2df8ddcdc0d5153cf2669e14e6ee272baba509c"; match = "DOOM.CE-Lite-*.zip"; unzip = @("DOOM64.CE*.ipk3", "DOOM64.CE*.pk3", "PSX*.ipk3", "PSX*.pk3", "DOOM64.bps", "LOST0?.bps"); page = "https://www.moddb.com/mods/doom-ce/downloads/doom-ce-400-lite-download"; what = "DOOM CE Lite - Doom 64, PSX Doom and PSX Final Doom (needs your Doom 64 and Doom II)" },
    @{ name = "PSXDOOM.CE.Addon.GFX.PBR.pk3"; dest = "doomce"; kind = "url"; url = "$mirror/psx-doom-ce-4.0-addons.zip"; sha256 = "1d2fe24d171ae3fa1d50d7526cbf08e9a2f6231bcfa9afd54f0a4859c76cf831"; match = "PSXDOOM.CE-*.zip"; unzip = @("PSXDOOM.CE*.ipk3", "PSXDOOM.CE*.pk3"); page = "https://www.moddb.com/mods/doom-ce/downloads/psx-doom-ce-400-full-download"; what = "PSX DOOM CE Full - its music, sound and texture addons" },
    @{ name = "PSXFINALDOOM.CE.Addon.GFX.PBR.pk3"; dest = "doomce"; kind = "url"; url = "$mirror/psx-final-doom-ce-4.0-addons.zip"; sha256 = "edf1fa4c10f0c6c1c3d26b102a4bfdf4fe68e96eda2c2b26e9b7546e2d47e74d"; match = "PSXFINALDOOM.CE-*.zip"; unzip = @("PSXFINALDOOM.CE*.ipk3", "PSXFINALDOOM.CE*.pk3"); page = "https://www.moddb.com/mods/doom-ce/downloads/psx-final-doom-ce-400-full-download"; what = "PSX FINAL DOOM CE Full - its music, sound and texture addons" },
    @{ name = "DOOM64.CE.Addon.SFX.HQ.pk3"; dest = "doomce"; kind = "url"; url = "$mirror/doom-64-ce-4.0-addons.zip"; sha256 = "d7b332d6bf312da882e32c7abdb24f7985a36d9877c6afd544fbfa53426a9d05"; match = "DOOM64.CE-*.zip"; unzip = @("DOOM64.CE*.ipk3", "DOOM64.CE*.pk3", "DOOM64.bps", "LOST0?.bps"); page = "https://www.moddb.com/mods/doom-ce/downloads/doom-64-ce-400-full-download"; what = "DOOM 64 CE Full - optional: its music, sound and texture addons" },
    # The 2x upscaled textures, once in the Full download and now an add-on of
    # its own (1.05 GB, GPL) - for Doom 64 and the PSX games and their map packs.
    @{ name = "DOOM64.CE.Addon.GFX.Upscale.pk3"; dest = "doomce"; kind = "url"; url = "$mirror/doom-ce-upscale-addon-4.zip"; sha256 = "8317ece54e10739a79f18a34d8d800309e2f09d2cf02fa46bd18b8c680db0e1a"; look = $true; match = "GFX.Upscale*.zip"; unzip = @("*Upscale*.pk3"); page = "https://www.moddb.com/mods/doom-ce/addons/upscale-addon"; what = "DOOM CE upscaled textures" },

    # --- voxels ----------------------------------------------------------------
    # NashGore NEXT (Nash Muhandes) and its official voxel gore, made with
    # Cheello "for use with Voxel Doom". Its readme: GZDoom 4.11.3 or later,
    # and the voxel pack loads AFTER nashgore.pk3.
    @{ name = "nashgore.pk3";                 kind = "url"; url = "$mirror/nashgore-next.zip"; sha256 = "36116155998c5b5f4e7527a39cfd6ca61ffa9c11599be712d168b9d1760f765e"; look = $true; match = "nashgore_next*";   unzip = "nashgore.pk3";                 page = "https://www.moddb.com/mods/nashgore-next/downloads/nashgore-next-v10";                what = "NashGore NEXT - gore that sticks to the walls (Nash Muhandes)" },
    @{ name = "nashgore_voxels_official.pk3"; kind = "url"; url = "$mirror/nashgore-next-voxels.zip"; sha256 = "148199dcc2c34e484cded7cdf132e773e6dbd5eaf4cc14b9d2914718ff0b0c79"; look = $true; match = "nashgore_voxels*"; unzip = "nashgore_voxels_official.pk3"; page = "https://www.moddb.com/mods/nashgore-next/downloads/nashgore-next-official-voxel-pack"; what = "NashGore NEXT official voxel pack - voxel gibs and blood (Cheello and Nash)" },
    @{ name = "VoxelDoom_v2.4.pk3"; kind = "url"; url = "$mirror/voxel-doom-ii-2.4.zip"; sha256 = "26aa27cec4f7be1d9491e70ef467dd2684ba3b8b463d1872b489d50043dc0ef8"; look = $true; match = "VoxelDoom_v2.4*"; unzip = "VoxelDoom_v2.4.pk3"; page = "https://www.moddb.com/mods/voxel-doom-ii/addons/voxel-doom-ii-with-parallax-textures"; what = "Voxel Doom II v2.4 - 3D voxel monsters, items and props (Cheello)" },
    # GRAU's .zip IS the pack - voxeldef.txt and the voxels at its top - so it
    # is saved as the .pk3 whole, not unpacked (unpacking finds nothing).
    @{ name = "RZXVOX-0.12.pk3"; kind = "url"; url = "http://rdw.xp3.biz/RDVOX/RZXVOX-0.12.zip"; what = "Hexen voxel items and props (GRAU / Renaissance Doomworks)" },
    # 0.11, not 0.12: RHVOX-0.12.zip downloads as 3,620 bytes with no central
    # directory - not a valid archive.
    @{ name = "RHVOX-0.11.pk3";  kind = "url"; url = "http://rdw.xp3.biz/RDVOX/RHVOX-0.11.zip"; what = "Heretic voxel items, props and projectiles (GRAU / Renaissance Doomworks)" },

    # --- classic megawads, from the Doomworld archive ------------------------
    # Every filename was probed for an exact match: the archive's search is
    # fuzzy and answers "dtwid" with dtwidrec.zip.
    @{ name = "scythe2.wad";      kind = "idgames"; file = "scythe2.zip";    unzip = @("*.wad"); what = "Scythe 2" },
    @{ name = "uacultra.wad";     kind = "idgames"; file = "uacultra.zip";   unzip = @("*.wad"); what = "UAC Ultra" },
    @{ name = "d2twid.wad";       kind = "idgames"; file = "d2twid.zip";     unzip = @("*.wad"); what = "Doom 2 the Way id Did" },
    @{ name = "btsx_e1a.wad";     kind = "idgames"; file = "btsx_e1.zip";    unzip = @("*.wad"); what = "Back to Saturn X, Episode 1" },
    @{ name = "btsx_e2a.wad";     kind = "idgames"; file = "btsx_e2.zip";    unzip = @("*.wad"); what = "Back to Saturn X, Episode 2" },
    @{ name = "aaliens_v1_2.wad"; kind = "idgames"; file = "aaliens.zip";    unzip = @("*.wad"); what = "Ancient Aliens" },
    @{ name = "sunlust.wad";      kind = "idgames"; file = "sunlust.zip";    unzip = @("*.wad"); what = "Sunlust" },
    @{ name = "valiant.wad";      kind = "idgames"; file = "valiant.zip";    unzip = @("*.wad"); what = "Valiant" },
    @{ name = "eviternity.wad";   kind = "idgames"; file = "eviternity.zip"; unzip = @("*.wad"); what = "Eviternity" },
    @{ name = "pl2.wad";          kind = "idgames"; file = "pl2.zip";        unzip = @("*.wad"); what = "Plutonia 2" },
    @{ name = "requiem.wad";      kind = "idgames"; file = "requiem.zip";    unzip = @("*.wad"); what = "Requiem" },
    @{ name = "MM.WAD";           kind = "idgames"; file = "mm_allup.zip";   unzip = @("*.wad"); what = "Memento Mori" },
    @{ name = "scythe.wad";       kind = "idgames"; file = "scythe.zip";     unzip = @("*.wad"); what = "Scythe" },
    @{ name = "vanguard.wad";     kind = "idgames"; file = "vanguard.zip";   unzip = @("*.wad"); what = "Vanguard" },
    @{ name = "lunatic.wad";      kind = "idgames"; file = "lunatic.zip";    unzip = @("*.wad"); what = "Lunatic" },
    @{ name = "wos.wad";          kind = "idgames"; file = "wos.zip";        unzip = @("*.wad"); what = "Whispers of Satan" },
    @{ name = "dtwid.wad";        kind = "idgames"; file = "dtwid.zip";      unzip = @("*.wad"); what = "Doom the Way id Did (needs Doom)" },

    # --- gameplay overhauls ----------------------------------------------------
    @{ name = "Beautiful_Doom_716.pk3"; kind = "url"; url = "https://github.com/jekyllgrim/Beautiful-Doom/releases/download/7.1.6/Beautiful_Doom_716.pk3"; what = "Beautiful Doom - vanilla made gorgeous" },
    @{ name = "brutalv21.pk3";          kind = "url"; url = "$mirror/brutal-doom-21.zip"; sha256 = "7785576a4187c20f1e1fea2b1682744fa9410597249a6e03830532eba7f95952"; match = "brutalv21*";        unzip = "brutalv21.pk3"; page = "https://www.moddb.com/mods/brutal-doom/downloads";        what = "Brutal Doom v21" },
    # A .7z holding PB-0_3_1-alpha.pk3.
    @{ name = "PB-0_3_1-alpha.pk3";     kind = "url"; url = "$mirror/project-brutality-0.3.1-alpha.zip"; sha256 = "984199b88ff260404e3e1bef7f92de5f8b2c1e6309fa979e087c1eb2a7328eee"; match = "Project_Brutality*"; unzip = "PB-0_3_1-alpha.pk3"; page = "https://www.moddb.com/mods/project-brutality/downloads/project-brutality-3"; what = "Project Brutality 0.3.1 alpha" },
    @{ name = "meatgrinderV2C.pk3";     kind = "url"; url = "$mirror/meatgrinder-2c.zip"; sha256 = "53eee816a2a697fc9bad34365f735a00691a9a21a667217a4aaf99aa2855f869"; match = "meatgrinder*";      unzip = "meatgrinderV2C.pk3"; page = "https://www.moddb.com/mods/brutal-doom";        what = "MeatGrinder" },

    # --- Doom 64 ---------------------------------------------------------------
    @{ name = "bd64gamev2.pk3"; kind = "url"; url = "$mirror/brutal-doom-64-game-2.zip"; sha256 = "801e9e721d953bcf24e2b78717f447be53e8caef17145c10ecadbca48542afda"; match = "bd64game*"; unzip = "bd64gamev2.pk3"; page = "https://www.moddb.com/mods/brutal-doom-64/downloads"; what = "Brutal Doom 64 - game" },
    @{ name = "bd64mapsV2.pk3"; kind = "url"; url = "$mirror/brutal-doom-64-maps-2.zip"; sha256 = "2a66831adba52406c9e9bd724a7031179eb3a56671406bd08f01c301df6fb2f8"; match = "bd64maps*"; unzip = "bd64mapsV2.pk3"; page = "https://www.moddb.com/mods/brutal-doom-64/downloads"; what = "Brutal Doom 64 - maps" },
    @{ name = "D64RTR[v1.5].WAD"; kind = "url"; url = "$mirror/doom-64-retribution-1.5.zip"; sha256 = "120759210a62f6186dc93cd7c53799585f3935561d9e4987d105908cca321d47"; match = "D64Retribution*"; unzip = "D64RTR*.WAD"; page = "https://www.moddb.com/mods/doom-64-retribution/downloads"; what = "Doom 64: Retribution" },

    # --- other total conversions -----------------------------------------------
    @{ name = "ZMC-BWFinal.pk3";                      kind = "url"; url = "$mirror/brutal-wolfenstein-3d-final.zip"; sha256 = "8536d55595ceda03aa539c15b03d81c175012e44f182d1afda2521ba0688fa3b"; match = "ZMC-BW*";      unzip = "ZMC-BWFinal.pk3"; page = "https://www.moddb.com/mods/brutal-wolfenstein-3d/downloads"; what = "Brutal Wolfenstein 3D" },
    @{ name = "ZBloody_Hell_v1915.pk3";               kind = "url"; url = "$mirror/zbloody-hell-1.9.15.zip"; sha256 = "f1b4c2d5d8c9e2af8ac4e518cac1d83b5710085f9cb11a5a96dd77ef388f93bf"; match = "ZBloody*";     unzip = "ZBloody_Hell_v1915.pk3"; page = "https://www.moddb.com/mods/zbloody-hell";                    what = "Blood (ZBloody Hell)" },
    @{ name = "ALIENSERADICATIONRC1RELEASE.wad";      kind = "url"; url = "$mirror/aliens-eradication-rc1.zip"; sha256 = "c79de139d8475806aafc69184f264d2c4b3352559c2dbcfa024fa57a623ab23a"; match = "ALIENSERAD*";  unzip = "ALIENSERADICATIONRC1RELEASE.wad"; page = "https://www.moddb.com/members/payload4367";              what = "Aliens: Eradication" },
    # The other half of the same release - Tracer, Loader, Knifespawner live here.
    @{ name = "KKALIENTRILOGLYPAYLDEDRC1RELEASE.pk3"; kind = "url"; url = "$mirror/aliens-eradication-rc1-textures-monsters.zip"; sha256 = "b9e60dc13e6fd400e98ff89c5cf1782dc53c97b54df5f6ae75bf38c6ec09dd8b"; match = "KKALIEN*";     unzip = "KKALIENTRILOGLYPAYLDEDRC1RELEASE.pk3"; page = "https://www.moddb.com/members/payload4367";              what = "Aliens: Eradication - textures and monsters" },
    @{ name = "hellonearthstarterpack.wad";           kind = "url"; url = "$mirror/hell-on-earth-starter-pack.zip"; sha256 = "8f2f3d205aab9840635bff9e07aad3b555efffc9584d8400e1ee6ae85c6e15d7"; match = "hellonearth*"; unzip = "hellonearthstarterpack.wad"; page = "https://www.moddb.com/mods/brutal-doom";      what = "Hell on Earth Starter Pack" },
    @{ name = "Ashes2063Enriched2_23.pk3";            kind = "url"; url = "$mirror/ashes-2063-enriched-2.23.zip"; sha256 = "f09a9a1a9ea72e563f6f2cd32b72f3a5d3e8f6f90655afb8fbbeb87c6ec55478"; match = "Ashes2063*";   unzip = "Ashes2063Enriched2_23.pk3"; page = "https://www.moddb.com/mods/ashes-2063/downloads";            what = "Ashes 2063" },
    @{ name = "DukeNukem3D_TC.pk3";                   kind = "url"; url = "$mirror/duke-nukem-3d-tc.zip"; sha256 = "969397bfc3983e33b0e13f1e8a0a8b71a3e23df3988dd88614e0a96d7b6f6075"; match = "DukeNukem3D*"; unzip = "DukeNukem3D_TC.pk3"; page = "https://www.moddb.com/mods/duke-nukem-3d-doom-tc/downloads"; what = "Duke Nukem 3D TC" },

    # --- sound and looks ---------------------------------------------------------
    # Per Kristian Risvik's hi-res Doom sounds, from his own site (not on
    # idgames). The zip holds the wad.
    @{ name = "pk_doom_sfx_20120224.wad"; kind = "url"; url = "https://www.perkristian.net/sounds/pk_doom_sfx.zip"; unzip = @("*.wad"); what = "Sounds Remastered (Per Kristian Risvik)" },
    # Andrew Hulshult's IDKFA, the WAD-only zip (86 MB; the full release with
    # FLACs is 719 MB). Not on idgames.
    @{ name = "IDKFAv2.wad"; kind = "url"; url = "$mirror/idkfa-soundtrack-2.zip"; sha256 = "2870cc328911d761450cb671e6f6c24ca3409c9bbe6eb4335ab96435c1eb7fcf"; match = "IDKFA*.zip"; unzip = "IDKFAv2.wad"; page = "https://www.moddb.com/downloads/idkfa-soundtrack-wad-only-version"; what = "IDKFA - Andrew Hulshult's Doom soundtrack" },
    @{ name = "NeuralUpscale2x_v1.0.pk3"; kind = "url"; url = "$mirror/neural-upscale-2x-1.0.zip"; sha256 = "024e803a9e7ab5ac00d8a6c7086841367d5008ee78dab5916e8747d5b3786f62"; look = $true; match = "NeuralUpscale*"; unzip = "NeuralUpscale2x_v1.0.pk3"; page = "https://www.moddb.com/mods/neural-upscale-texture-pack"; what = "Neural Upscale textures" }
)

# Everything that loads on top of a Doom II - the re-release's if there is
# one (it carries the widescreen art and the support wad), else the DOS one.
# The first file is the mod itself; the rest are loaded when present.
$onDoom2 = @(
    @{ name = "Scythe 2";                     files = @("mods\scythe2.wad") },
    @{ name = "UAC Ultra";                    files = @("mods\uacultra.wad") },
    @{ name = "Doom 2 the Way id Did";        files = @("mods\d2twid.wad") },
    @{ name = "Back to Saturn X - Episode 1"; files = @("mods\btsx_e1a.wad", "mods\btsx_e1b.wad") },
    @{ name = "Back to Saturn X - Episode 2"; files = @("mods\btsx_e2a.wad", "mods\btsx_e2b.wad") },
    @{ name = "Ancient Aliens";               files = @("mods\aaliens_v1_2.wad") },
    @{ name = "Sunlust";                      files = @("mods\sunlust.wad") },
    @{ name = "Valiant";                      files = @("mods\valiant.wad") },
    @{ name = "Eviternity";                   files = @("mods\eviternity.wad") },
    @{ name = "Plutonia 2";                   files = @("mods\pl2.wad") },
    @{ name = "Requiem";                      files = @("mods\requiem.wad", "mods\REQMUS.WAD") },
    @{ name = "Memento Mori";                 files = @("mods\MM.WAD", "mods\MMMUS.WAD") },
    @{ name = "Scythe";                       files = @("mods\scythe.wad") },
    @{ name = "Vanguard";                     files = @("mods\vanguard.wad") },
    @{ name = "Lunatic";                      files = @("mods\lunatic.wad") },
    @{ name = "Whispers of Satan";            files = @("mods\wos.wad") },
    @{ name = "Hell on Earth Starter Pack";   files = @("mods\hellonearthstarterpack.wad") },
    @{ name = "Brutal Doom II";               files = @("mods\brutalv21.pk3") },
    @{ name = "Project Brutality";            files = @("mods\PB-0_3_1-alpha.pk3") },
    @{ name = "MeatGrinder";                  files = @("mods\meatgrinderV2C.pk3") },
    @{ name = "Beautiful Doom";               files = @("mods\Beautiful_Doom_716.pk3") },
    @{ name = "Brutal Doom 64";               files = @("mods\bd64gamev2.pk3", "mods\bd64mapsV2.pk3") },
    @{ name = "Doom 64 Retribution";          files = @("mods\D64RTR[v1.5].WAD") },
    @{ name = "Brutal Wolfenstein";           files = @("mods\ZMC-BWFinal.pk3") },
    @{ name = "Blood - ZBloody Hell";         files = @("mods\ZBloody_Hell_v1915.pk3") },
    # The .pk3 first: the .wad's DECORATE replaces the tracers it defines.
    @{ name = "Aliens - Eradication";         files = @("mods\KKALIENTRILOGLYPAYLDEDRC1RELEASE.pk3", "mods\ALIENSERADICATIONRC1RELEASE.wad") },
    @{ name = "Ashes 2063";                   files = @("mods\Ashes2063Enriched2_23.pk3") },
    @{ name = "Duke Nukem 3D TC";             files = @("mods\DukeNukem3D_TC.pk3") }
)
$games += @(
    @{ name = "Castlevania - Simon's Destiny"; iwad = "wads\Castlevania.ipk3" },
    @{ name = "Doom - the Way id Did";         iwad = "wads\DOOM.WAD"; files = @("mods\dtwid.wad") },
    @{ name = "Doom - the Way id Did";         iwad = "kex\doom.wad";  files = @("mods\dtwid.wad") },
    # Brutal Doom is gameplay only and plays the maps under it: Doom's here,
    # Doom II's as "Brutal Doom II".
    @{ name = "Brutal Doom";                   iwad = "kex\doom.wad";  files = @("mods\brutalv21.pk3") },
    @{ name = "Brutal Doom";                   iwad = "wads\DOOM.WAD"; files = @("mods\brutalv21.pk3") }
)
foreach ($m in $onDoom2) {
    foreach ($base in "kex\doom2.wad", "wads\DOOM2.WAD") {
        $games += @{ name = $m.name; iwad = $base; files = @($m.files[0]); optional = @($m.files | Select-Object -Skip 1) }
    }
}

$hhPatches = @("hh_patch_common.pk3", "hh_patch_heretic.pk3", "hh_patch_fr.pk3", "hh_patch_hexen.pk3", "hh_patch_hexdd.pk3", "hh_patch_vog.pk3")

# Files an IWAD needs in its own folder, copied into wads\ with it from the
# same folder: Strife's speech. (The re-releases' companions travel with
# their whole set into kex\.)
$companions = @("voices.wad")

# The re-release sets, known by a file only they have. Everything with a
# .wad name in that folder is the set - except a developer's test map.
$kexMarkers = @("id24res.wad", "heretic_ex.wad", "hexen_ex.wad")
$kexSkip = @("hextest.wad")

$wantedWads = @{}
foreach ($g in $games) {
    foreach ($f in @($g.iwad) + @($g.files) + @($g.needs)) {
        if ($f -and $f.StartsWith("wads\")) { $wantedWads[$f.Substring(5).ToLower()] = $true }
    }
}

# Nightdive's DOOM64.WAD: not a game UZDoom can start, but the file DOOM 64
# CE's patch is made from.
$wantedWads["doom64.wad"] = $true

$wadsDir = if ($LinkWadsCheck) { $LinkWadsCheck } elseif ($LinkWads) { Join-Path $Install $LinkWads } else { Join-Path $Install "wads" }
$kexDir = Join-Path $Install "kex"
$modsDir = Join-Path $Install "mods"
$doomceDir = Join-Path $Install "doomce"
if (-not $LinkWads) {
    foreach ($d in $wadsDir, $kexDir, $modsDir, $doomceDir) { New-Item -ItemType Directory -Force $d | Out-Null }
}
if (-not (Test-Path $wadsDir)) { throw "no wads folder at $wadsDir" }

Say ("Doom VR setup, " + (Get-Date -Format "yyyy-MM-dd HH:mm") + ", install " + $Install)

# ---------------------------------------------------------------------------
# Where to look, best first. A folder the player named beats everything.
# Then, within a store's folder: the original DOS files (base\, or the dos\
# a newer release keeps beside its own); then anything else; then the Unity
# port's; then the censored BFG Edition's. A re-release set never feeds
# wads\ - it is copied whole into kex\ and has launchers of its own.
# ---------------------------------------------------------------------------
function Rank([string]$path, [int]$base) {
    $p = $path.ToLower()
    # BFG first: it keeps its censored WADs under base\ too.
    if ($p -match 'bfg') { return $base + 4 }
    if ($p -match 'streamingassets') { return $base + 3 }
    if ($p -match '\\dos\\' -or $p -match '\\base\\') { return $base + 1 }
    return $base + 2
}

$found = @{}     # wads\ candidates: lower-case name -> @{ path; rank; from; time }
$kexFound = @{}  # kex\ candidates: lower-case name -> @{ path; from; time }
function Better($old, [int]$rank, $time) {
    # Better place wins; in an equally good place, the newer file. Steam's
    # Doom II and Ultimate Doom both carry a re-release doom2.wad, and the
    # one first found was the older build.
    if ($old -eq $null) { return $true }
    if ($old.rank -lt $rank) { return $false }
    if ($old.rank -eq $rank -and $old.time -ge $time) { return $false }
    return $true
}
function Consider([string]$path, [int]$rank, [string]$from) {
    $n = [System.IO.Path]::GetFileName($path).ToLower()
    if (-not $wantedWads.ContainsKey($n)) { return }
    $time = (Get-Item -LiteralPath $path).LastWriteTimeUtc
    if (Better $found[$n] $rank $time) { $found[$n] = @{ path = $path; rank = $rank; from = $from; time = $time } }
}
function ConsiderKex([string]$path, [string]$from) {
    $n = [System.IO.Path]::GetFileName($path).ToLower()
    if ($kexSkip -contains $n) { return }
    $time = (Get-Item -LiteralPath $path).LastWriteTimeUtc
    $old = $kexFound[$n]
    if ($old -eq $null -or $old.time -lt $time) { $kexFound[$n] = @{ path = $path; from = $from; time = $time } }
}

# Doom-family folder names; the big modern games are never searched (and
# Doom Eternal's classic WADs are ones UZDoom misdetects).
$familyName = '(?i)doom|heretic|hexen|strife|chex|hacx|freedoom|square|blasphem|osiris|triad'
$notThese   = '(?i)eternal|dark ages|^doom$|doom vfr|doom 3$|doom3$'

function SearchTree([string]$root, [int]$base, [string]$from) {
    if (-not (Test-Path -LiteralPath $root)) { return }
    $files = @(Get-ChildItem -LiteralPath $root -File -Recurse -Depth 3 -ErrorAction SilentlyContinue)
    $setDirs = @{}
    foreach ($f in $files) { if ($kexMarkers -contains $f.Name.ToLower()) { $setDirs[$f.DirectoryName.ToLower()] = $true } }
    foreach ($f in $files) {
        if ($setDirs.ContainsKey($f.DirectoryName.ToLower())) {
            if ($f.Extension -ieq ".wad") { ConsiderKex $f.FullName $from }
        } else {
            Consider $f.FullName (Rank $f.FullName $base) $from
        }
    }
}

# What is already in wads\ is the best of all: never replaced.
if (-not $LinkWads) {
    Get-ChildItem -LiteralPath $wadsDir -File -ErrorAction SilentlyContinue |
        ForEach-Object { Consider $_.FullName -1 "already in wads" }
}

if ($LinkWads) {
    Get-ChildItem -LiteralPath $wadsDir -File | ForEach-Object { Consider $_.FullName 0 "linked" }
} else {
    foreach ($s in $Search) {
        if (-not $s) { continue }
        if (Test-Path -LiteralPath $s -PathType Leaf) { $s = Split-Path -Parent $s }
        if (Test-Path -LiteralPath $s) {
            Say "Looking in $s"
            SearchTree $s 0 $s
            # A dropped folder may hold the Heretic + Hexen patch and catalogue
            # files already - an old mods folder, or a whole earlier DoomVR or
            # other Doom port's install with its mods\ and wads\. Taken by exact name.
            $catNames = @{}
            foreach ($c in $catalogue) { $catNames[$c.name.ToLower()] = $(if ($c.dest) { $c.dest } else { "mods" }) }
            foreach ($h in $hhPatches) { $catNames[$h.ToLower()] = "mods" }
            foreach ($x in "btsx_e1b.wad", "btsx_e2b.wad", "reqmus.wad", "mmmus.wad") { $catNames[$x] = "mods" }
            Get-ChildItem -LiteralPath $s -File -Recurse -Depth 2 -ErrorAction SilentlyContinue | Where-Object { $catNames.ContainsKey($_.Name.ToLower()) } | ForEach-Object {
                $destDir = Join-Path $Install $catNames[$_.Name.ToLower()]
                $dst = Join-Path $destDir $_.Name
                if (-not (Test-Path -LiteralPath $dst)) { Copy-Item -LiteralPath $_.FullName -Destination $dst; Say ("Copied " + $catNames[$_.Name.ToLower()] + "\" + $_.Name + "  (from " + $s + ")") }
            }
        }
        else { Say "No such folder: $s" }
    }

    # A path joined without asking the drive: Join-Path stops the whole search
    # when a Steam library is on a drive that is no longer there (an unplugged
    # USB drive stays in libraryfolders.vdf); Test-Path on the result is False.
    function Combine-Path([string]$a, [string]$b) {
        try { return [System.IO.Path]::Combine($a, $b) } catch { return "" }
    }

    if (-not $NoSteam) {
        $steam = $SteamRoot
        if (-not $steam) {
            try { $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch { }
        }
        if ($steam) {
            $steam = $steam.Replace('/', '\')
            $libs = New-Object System.Collections.Generic.List[string]
            $libs.Add($steam)
            foreach ($vdf in @((Combine-Path $steam "steamapps\libraryfolders.vdf"), (Combine-Path $steam "config\libraryfolders.vdf"))) {
                if (-not $vdf -or -not (Test-Path -LiteralPath $vdf)) { continue }
                foreach ($line in Get-Content -LiteralPath $vdf) {
                    if ($line -match '^\s*"path"\s*"(.+)"\s*$') {
                        $p = $Matches[1].Replace('\\', '\')
                        # The registry and the .vdf spell the same folder in different case.
                        if (-not ($libs | Where-Object { $_ -ieq $p })) { $libs.Add($p) }
                    }
                }
            }
            foreach ($lib in $libs) {
                $common = Combine-Path $lib "steamapps\common"
                if (-not $common -or -not (Test-Path -LiteralPath $common)) { continue }
                Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match $familyName -and $_.Name -notmatch $notThese } |
                    ForEach-Object { Say ("Steam: " + $_.FullName); SearchTree $_.FullName 10 ("Steam: " + $_.Name) }
            }
        } else {
            Say "Steam: not installed"
        }
    }

    if (-not $NoGog) {
        foreach ($key in @("HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games", "HKLM:\SOFTWARE\GOG.com\Games")) {
            if (-not (Test-Path $key)) { continue }
            Get-ChildItem $key -ErrorAction SilentlyContinue | ForEach-Object {
                $g = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
                if ($g -and $g.path -and ("" + $g.gameName) -match $familyName -and ("" + $g.gameName) -notmatch $notThese) {
                    Say ("GOG: " + $g.gameName + " - " + $g.path)
                    SearchTree $g.path 10 ("GOG: " + $g.gameName)
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Copy - never over a file that is already there.
# ---------------------------------------------------------------------------
$copied = 0
function CopyIn([string]$src, [string]$dir, [string]$label, [string]$from) {
    $dst = Join-Path $dir ([System.IO.Path]::GetFileName($src))
    if (Test-Path -LiteralPath $dst) { return }
    Copy-Item -LiteralPath $src -Destination $dst
    $mb = (Get-Item -LiteralPath $dst).Length / 1MB
    Say ("Copied {0}\{1}  {2:N0} MB  (from {3})" -f $label, [System.IO.Path]::GetFileName($src), $mb, $from)
    $script:copied++
}
if (-not $LinkWads) {
    foreach ($n in @($found.Keys | Sort-Object)) {
        $f = $found[$n]
        if ($f.rank -lt 0) { continue }
        CopyIn $f.path $wadsDir "wads" $f.from
        $srcDir = Split-Path -Parent $f.path
        foreach ($c in $companions) {
            $cp = Join-Path $srcDir $c
            if (Test-Path -LiteralPath $cp) { CopyIn $cp $wadsDir "wads" $f.from }
        }
    }
    foreach ($n in @($kexFound.Keys | Sort-Object)) { CopyIn $kexFound[$n].path $kexDir "kex" $kexFound[$n].from }
    if ($copied -eq 0) { Say "Nothing new to copy." }
}

function PathOf([string]$rel) {
    if ($rel.StartsWith("wads\")) { return (Join-Path $wadsDir $rel.Substring(5)) }
    return (Join-Path $Install $rel)
}
function Has([string]$rel) { return (Test-Path -LiteralPath (PathOf $rel)) }

# ---------------------------------------------------------------------------
# Heretic + Hexen (2025): trzejos/heretic-hexen-zdpatches v0.2.0, written for
# UZDoom 5, from its GitHub release. The re-release's new actors are JSON
# inside its IWADs, which UZDoom does not read; the patch defines them. It
# carries no licence, so it is only ever fetched here, onto the player's
# machine - never put in a DoomVR download. Only fetched for a player who
# owns the re-release.
# ---------------------------------------------------------------------------
function Is-Zip([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $fs = [IO.File]::OpenRead($path)
    try { $b = New-Object byte[] 2; [void]$fs.Read($b, 0, 2); return ($b[0] -eq 0x50 -and $b[1] -eq 0x4B) } finally { $fs.Close() }
}
if (-not $LinkWads -and ((Has "kex\heretic.wad") -or (Has "kex\hexen.wad"))) {
    $need = @($hhPatches | Where-Object { -not (Has ("mods\" + $_)) })
    if ($need.Count -gt 0 -and $NoDownload) {
        Say "Heretic + Hexen (2025) patch: not fetched (-NoDownload)"
    } elseif ($need.Count -gt 0) {
        Say "Fetching the Heretic + Hexen (2025) patch (trzejos/heretic-hexen-zdpatches) ..."
        try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
        foreach ($p in $need) {
            $dst = Join-Path $modsDir $p
            $tmp = $dst + ".part"
            try {
                Invoke-WebRequest -Uri ($PatchBase.TrimEnd('/') + "/" + $p) -OutFile $tmp -UseBasicParsing -UserAgent "DoomVR-setup"
                # A web page saved under a .pk3 name is the failure to catch
                # here; it would otherwise be found at level load.
                if (-not (Is-Zip $tmp)) { throw "the server sent something that is not a .pk3" }
                Move-Item -LiteralPath $tmp -Destination $dst
                Say ("Fetched mods\{0}  {1:N0} KB" -f $p, ((Get-Item -LiteralPath $dst).Length / 1KB))
            } catch {
                Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
                Say ("Could not fetch {0}: {1}" -f $p, $_.Exception.Message)
                Say "  The 2025 Heretic + Hexen games need it. Run SETUP.bat again when online."
            }
        }
    }
}

# ---------------------------------------------------------------------------
# The mod catalogue: fetched from where each author publishes it. Skipped
# for what is already there; nothing is replaced.
# ---------------------------------------------------------------------------
function Expand-Wanted([string]$archive, $want, [string]$dir) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    $out = @()
    try {
        foreach ($e in $zip.Entries) {
            if (-not $e.Name) { continue }
            $hit = $false
            foreach ($w in @($want)) { if ($e.Name -like $w) { $hit = $true; break } }
            if (-not $hit) { continue }
            $to = Join-Path $dir $e.Name
            if (Test-Path -LiteralPath $to) { continue }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $to, $false)
            $out += $to
        }
    } finally { $zip.Dispose() }
    return ,@($out)
}
# The Doomworld archive: its API knows the directory for a filename. The
# search is fuzzy, so only an exact filename match is taken.
function Resolve-Idgames([string]$file) {
    $api = "https://www.doomworld.com/idgames/api/api.php?action=search&query=" +
           [Uri]::EscapeDataString(($file -replace '\.zip$', '')) + "&type=filename&out=json"
    $r = Invoke-RestMethod -Uri $api -UseBasicParsing -UserAgent "DoomVR-setup"
    $f = @($r.content.file) | Where-Object { $_.filename -eq $file } | Select-Object -First 1
    if (-not $f) { return $null }
    return ($f.dir + $f.filename)
}
$idgamesMirrors = @("https://www.gamers.org/pub/idgames", "https://ftp.fu-berlin.de/pc/games/idgames")

# One file out of an archive under another name than the catalogue's (a
# release renamed inside its zip): give it the catalogue's name, which the
# launchers and the next run look for.
function Settle-Name($p, [string]$dir, $got) {
    $final = Join-Path $dir $p.name
    if ((Test-Path -LiteralPath $final) -or @($got).Count -ne 1) { return }
    Move-Item -LiteralPath @($got)[0] -Destination $final
}

# A manual download (ModDB and the like), taken from the Downloads folder:
# unzipped, un-7zipped or copied into place. True when the file is now there.
function Take-Manual($p) {
    $got = @()
    $dir = Join-Path $Install $(if ($p.dest) { $p.dest } else { "mods" })
    if (Test-Path -LiteralPath (Join-Path $dir $p.name)) { return $true }
    $cand = Get-ChildItem -LiteralPath $Downloads -Filter $p.match -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -notin ".crdownload", ".part", ".tmp" } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cand) { return $false }
    # Still arriving: some browsers write the download under its final name
    # as it comes in, and a half-written zip reads as corrupt. Wait until it
    # has not changed for a few seconds.
    if (((Get-Date) - $cand.LastWriteTime).TotalSeconds -lt 8) { return $false }
    # Windows may not move the time while the file is open, so also: the same
    # size as the last look, and no one else holding it open.
    if (-not $script:lastSize) { $script:lastSize = @{} }
    $seen = $script:lastSize[$cand.FullName]
    $script:lastSize[$cand.FullName] = $cand.Length
    if ($null -ne $seen -and $seen -ne $cand.Length) { return $false }
    try { ([IO.File]::Open($cand.FullName, "Open", "Read", "None")).Dispose() } catch { return $false }
            try {
                $want = if ($p.unzip) { $p.unzip } else { $p.name }
                if ($cand.Extension -ieq ".zip" -and $p.name -notlike "*.zip") {
                    $got = Expand-Wanted $cand.FullName $want $dir
                    if ($got.Count -eq 0) { $got = Expand-Wanted $cand.FullName @("*.wad", "*.pk3", "*.ipk3") $dir }
                    if ($got.Count -eq 0) { throw "nothing usable inside it" }
                } elseif ($cand.Extension -ieq ".7z") {
                    # Windows' own tar (Windows 10 1803 and later) reads 7z.
                    $tmp7 = Join-Path $env:TEMP ("doomvr_7z_" + [guid]::NewGuid().ToString("N"))
                    New-Item -ItemType Directory $tmp7 | Out-Null
                    try {
                        cmd /c ("tar -xf `"" + $cand.FullName + "`" -C `"" + $tmp7 + "`" 2>nul") | Out-Null
                        if ($LASTEXITCODE -ne 0) { throw "Windows could not unpack the .7z" }
                        $got = @()
                        foreach ($f in Get-ChildItem -LiteralPath $tmp7 -File -Recurse) {
                            $hit = $false
                            foreach ($w in @($want)) { if ($f.Name -like $w) { $hit = $true } }
                            if (-not $hit) { continue }
                            $to = Join-Path $dir $f.Name
                            if (-not (Test-Path -LiteralPath $to)) { Move-Item -LiteralPath $f.FullName -Destination $to; $got += $to }
                        }
                        if ($got.Count -eq 0) { throw "nothing usable inside it" }
                    } finally { Remove-Item -LiteralPath $tmp7 -Recurse -Force -ErrorAction SilentlyContinue }
                } elseif ($cand.Extension -ieq [IO.Path]::GetExtension($p.name)) {
                    Copy-Item -LiteralPath $cand.FullName -Destination (Join-Path $dir $p.name)
                } else {
                    # Never copy an archive under a game file's name: the 7z
                    # of Project Brutality was once taken as a .pk3 this way.
                    throw ("it is a " + $cand.Extension + " file, and setup cannot open that kind")
                }
                Settle-Name $p $dir $got
                Say ("Took {0} from Downloads  ({1})" -f $p.name, $p.what)
                $script:fetched++
                return $true
            } catch {
                # Once per file as it stands, not on every pass of the wait.
                $key = $cand.FullName + "|" + $cand.Length + "|" + $cand.LastWriteTime.Ticks
                if (-not $script:saidBad) { $script:saidBad = @{} }
                if (-not $script:saidBad.ContainsKey($key)) {
                    $script:saidBad[$key] = $true
                    Say ("{0} is in Downloads but could not be used: {1}" -f $p.name, $_.Exception.Message)
                }
                return $false
            }
    return $false
}

if (-not $LinkWads) {
    $pending = @()
    $fetched = 0
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
    # How many there are to fetch, so each line can say "12 of 63".
    $toFetch = @($catalogue | Where-Object { $_.kind -ne "manual" -and -not (Test-Path -LiteralPath (Join-Path (Join-Path $Install $(if ($_.dest) { $_.dest } else { "mods" })) $_.name)) }).Count
    if ($NoDownload) { $toFetch = 0 }
    $fetchN = 0
    if ($toFetch -gt 0) { Say ("Downloading {0} free games and mods..." -f $toFetch) }
    foreach ($p in $catalogue) {
        $dir = Join-Path $Install $(if ($p.dest) { $p.dest } else { "mods" })
        if (Test-Path -LiteralPath (Join-Path $dir $p.name)) { continue }

        if ($p.kind -eq "manual") {
            if (-not (Take-Manual $p)) { $pending += $p }
            continue
        }
        if ($NoDownload) { continue }

        $url = $p.url
        $rel = $null
        if ($p.kind -eq "idgames") {
            try { $rel = Resolve-Idgames $p.file } catch { $rel = $null }
            if (-not $rel) { Say ("{0}: not found in the idgames archive" -f $p.name); continue }
            $url = $idgamesMirrors[0] + "/" + $rel
        }
        # This run's own name: two setups at once must not share a file.
        $tmp = Join-Path $env:TEMP ("doomvr_{0}_{1}" -f $PID, [IO.Path]::GetFileName($url))
        $fetchN++
        Say ("Downloading {0} of {1}: {2}..." -f $fetchN, $toFetch, ($p.what -replace ' - .*$', ''))
        $ok = $false
        foreach ($attempt in 1..2) {
            try {
                Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -UserAgent "DoomVR-setup"
                if (-not (Is-Zip $tmp)) { throw "not an archive - the server sent a page instead of the file" }
                if ($p.sha256 -and (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash -ne $p.sha256.ToUpper()) { throw "not the file expected (its fingerprint differs)" }
                $ok = $true; break
            } catch {
                if ($p.kind -eq "idgames" -and $attempt -eq 1) { $url = $idgamesMirrors[1] + "/" + $rel; continue }
                if ($attempt -eq 1) { Start-Sleep -Seconds 3; continue }	# a dropped connection or a busy file: once more
                Say ("{0}: download FAILED ({1}) - by hand: {2}" -f $p.name, $_.Exception.Message, $url)
                break
            }
        }
        if (-not $ok) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            # A mirrored mod the mirror cannot give: its author's page, one click.
            if ($p.page -and $p.match -and -not (Take-Manual $p)) { $pending += $p }
            continue
        }
        try {
            if ($p.unzip) {
                $got = Expand-Wanted $tmp $p.unzip $dir
                if ($got.Count -eq 0) { throw "nothing matching inside the archive" }
                Settle-Name $p $dir $got
            } else { Move-Item -LiteralPath $tmp -Destination (Join-Path $dir $p.name) }
            Say ("Fetched {0}  ({1}, {2:N0} MB)" -f $p.name, $p.what, ((Get-ChildItem -LiteralPath $dir -Filter $p.name -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB))
            $fetched++
        } catch {
            Say ("{0}: FAILED ({1})" -f $p.name, $_.Exception.Message)
        } finally {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        }
    }
    # What still needs a click, for the VR Setup and the Game Or Die Installer
    # (it offers the Modern VR look's own): look|file|what|page, one a line.
    $manualList = Join-Path $Install "manual-downloads.txt"
    if ($pending.Count -gt 0) {
        [IO.File]::WriteAllLines($manualList, [string[]]@($pending | ForEach-Object { ("{0}|{1}|{2}|{3}" -f $(if ($_.look) { "look" } else { "extra" }), $_.name, $_.what, $_.page) }))
    } elseif (Test-Path -LiteralPath $manualList) { Remove-Item -LiteralPath $manualList -Force }
    $lookPending = @($pending | Where-Object { $_.look })
    if ($lookPending.Count -gt 0) {
        Say ""
        Say ("The Modern VR look needs {0} one-click download(s) - their sites refuse scripted downloads:" -f $lookPending.Count)
        foreach ($p in $lookPending) { Say ("  {0}`r`n      {1}" -f $p.what, $p.page) }
    }
    if ($LookOnly) { $pending = $lookPending }
    if ($pending.Count -gt 0) {
        $extra = @($pending | Where-Object { -not $_.look })
        if ($extra.Count -gt 0) {
            Say ""
            Say ("{0} more mods need one click each - their sites refuse scripted downloads." -f $extra.Count)
            Say "Download any you want, leave the file in your Downloads folder, and run Setup again:"
            foreach ($p in $extra) { Say ("  {0}`r`n      {1}" -f $p.what, $p.page) }
        }
        # The Modern VR look's own first, asked on their own; then the rest.
        if ($lookPending.Count -gt 0 -and -not $LookOnly) { $pending = @($lookPending) + @($extra) }

        # Guided: open the pages, then take each file the moment it lands in
        # Downloads - no need to run setup again. Only asked of a person at a
        # console; unattended runs just list them.
        $interactive = -not $Unattended -and [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
        if ($interactive -or $GuidedTest) {
            Write-Host ""
            if ($LookOnly -or @($pending | Where-Object { -not $_.look }).Count -eq 0) {
                Write-Host "Open the Modern VR look's download pages now (Voxel Doom, NashGore, upscaled textures)?" -ForegroundColor Yellow
                Write-Host "Click Download on each; setup picks each file up as it finishes. [Y] yes  [N] no (default in 20 s)" -ForegroundColor Yellow
            } else {
                Write-Host "Open these download pages in your browser now? The first ones are the Modern VR look." -ForegroundColor Yellow
                Write-Host "Click Download on each one you want; setup picks each file up as it finishes. [Y] yes  [N] no (default in 20 s)" -ForegroundColor Yellow
            }
            $answer = if ($GuidedTest) { "Y" } else { "N" }
            $until = if ($GuidedTest) { Get-Date } else { (Get-Date).AddSeconds(20) }
            while ((Get-Date) -lt $until) {
                if ([Console]::KeyAvailable) { $answer = [Console]::ReadKey($true).KeyChar.ToString().ToUpper(); break }
                Start-Sleep -Milliseconds 200
            }
            if ($answer -eq "Y") {
                $opened = @{}
                foreach ($p in $pending) { if (-not $opened.ContainsKey($p.page)) { if (-not $GuidedTest) { Start-Process $p.page; Start-Sleep -Milliseconds 400 }; $opened[$p.page] = $true } }
                Say ("Opened {0} pages. Waiting for the downloads - press any key to stop waiting." -f $opened.Count)
                $left = [System.Collections.ArrayList]@($pending)
                $giveUp = if ($GuidedTest) { (Get-Date).AddSeconds($GuidedTestSeconds) } else { [DateTime]::MaxValue }
                while ($left.Count -gt 0 -and (Get-Date) -lt $giveUp) {
                    if (-not $GuidedTest -and [Console]::KeyAvailable) { [void][Console]::ReadKey($true); break }
                    foreach ($p in @($left)) { if (Take-Manual $p) { [void]$left.Remove($p) } }
                    Start-Sleep -Seconds 2
                }
                if ($left.Count -gt 0) { Say ("Still to download: " + (($left | ForEach-Object { $_.what }) -join "; ")) }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# The Doom games' held weapons used to be Voxel Doom's own pickup voxels,
# pointed at by a MODELDEF of ours (mods\doomvr-voxel-weapons.pk3). Since
# they are our own animated Doom guns, made below by gdh_setup. An
# earlier setup's copy of the old pack is ours, and goes.
# ---------------------------------------------------------------------------
if (Has "mods\doomvr-voxel-weapons.pk3") {
    Remove-Item -LiteralPath (PathOf "mods\doomvr-voxel-weapons.pk3") -Force
    Say "Doom guns: removed the old Voxel Doom hand pack (our animated Doom guns replace it)"
}

# ---------------------------------------------------------------------------
# Game Or Die Hands (setup\gdh): our hand models, and made HERE from the
# player's own games - never shipped - each game's hand skin (hands.pk3,
# beside the engine) and the held weapons: Doom's own eight, animated (their
# moving parts - slide, pump, break-open, barrels, chain), Legacy of Rust,
# Heretic, Hexen, Strife and Chex, and MODELDEFs naming RHVOX's and RZXVOX's
# own weapon voxels (the
# doomvr-voxel-weapons-*.pk3 in mods\, loaded in Modern style by the table
# below). gdh_setup finds each game by its file name and says in its report
# what it made and skipped. Our earlier output is replaced, never merged.
# ---------------------------------------------------------------------------
$gdh = Join-Path $PSScriptRoot "gdh\gdh_setup.exe"
if (Test-Path -LiteralPath $gdh) {
    $gdhOut = Join-Path $Install "gdh-out.new"
    if (Test-Path -LiteralPath $gdhOut) { Remove-Item -LiteralPath $gdhOut -Recurse -Force }
    New-Item -ItemType Directory -Force $gdhOut | Out-Null
    $gdhArgs = @("doom", "--wads", $wadsDir, "--wads", (Join-Path $Install "kex"), "--wads", (Join-Path $Install "mods"),
                 "--hands", (Join-Path $PSScriptRoot "gdh"), "--engine", $Install, "--out", $gdhOut)
    $gdhLog = & $gdh @gdhArgs 2>&1
    [IO.File]::WriteAllLines((Join-Path $Install "log-hands.txt"), [string[]]@($gdhLog))
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath (Join-Path $gdhOut "hands.pk3"))) {
        New-Item -ItemType Directory -Force (PathOf "mods") | Out-Null
        foreach ($old in @(Get-ChildItem -LiteralPath (PathOf "mods") -Filter "doomvr-voxel-weapons-*.pk3")) { Remove-Item -LiteralPath $old.FullName -Force }
        Move-Item -LiteralPath (Join-Path $gdhOut "hands.pk3") -Destination (Join-Path $Install "hands.pk3") -Force
        # The Game Or Die gloves (Hands style: Game Or Die), our own art: added
        # to hands.pk3 beside gdh_setup's styles, laid out as they are.
        $godDir = Join-Path $PSScriptRoot "gdh\gameordie"
        if (Test-Path -LiteralPath (Join-Path $godDir "hand_r.gdh")) {
            Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
            $zh = [IO.Compression.ZipFile]::Open((Join-Path $Install "hands.pk3"), 'Update')
            try {
                foreach ($pair in @(@("hand_r.gdh", "doomvr/hands/gameordie/hand_r.gdh"), @("hand_l.gdh", "doomvr/hands/gameordie/hand_l.gdh"),
                                    @("skin.png", "doomvr/hands/gameordie/skin.png"),
                                    @("normal.png", "materials/normalmaps/doomvr/hands/gameordie/skin.png"),
                                    @("specular.png", "materials/specular/doomvr/hands/gameordie/skin.png"))) {
                    $old = $zh.GetEntry($pair[1]); if ($old) { $old.Delete() }
                    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zh, (Join-Path $godDir $pair[0]), $pair[1])
                }
            } finally { $zh.Dispose() }
        }
        $made = @(Get-ChildItem -LiteralPath $gdhOut -Filter "doomvr-voxel-weapons-*.pk3")
        foreach ($p in $made) { Move-Item -LiteralPath $p.FullName -Destination (PathOf ("mods\" + $p.Name)) -Force }
        if (Test-Path -LiteralPath (Join-Path $gdhOut "gdh_doom_report.txt")) { Move-Item -LiteralPath (Join-Path $gdhOut "gdh_doom_report.txt") -Destination (Join-Path $Install "log-hands-report.txt") -Force }
        Say ("Hands: made your hands and {0} held-weapon pack(s) from your games (log-hands-report.txt)" -f $made.Count)
    } else {
        Say ("Hands: gdh_setup did not finish (exit {0}) - see log-hands.txt; VR hands stay off" -f $LASTEXITCODE)
    }
    Remove-Item -LiteralPath $gdhOut -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# Game Or Die's own Doom guns: our models (no game data), shipped in
# setup\gdh and copied into mods\ - always, so an update brings the new ones.
# ---------------------------------------------------------------------------
# With them, the packs that put them on the weapons of DOOM CE's games and
# Doom 64: Retribution (doomvr-weapons-<game>-b.pk3: their class and sprite
# names only, from tools/replacer-guns.py).
$ourGuns = Join-Path $PSScriptRoot "gdh\doomvr-weapons-doom-b.pk3"
if (Test-Path -LiteralPath $ourGuns) {
    New-Item -ItemType Directory -Force (PathOf "mods") | Out-Null
    foreach ($p in @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "gdh") -Filter "doomvr-weapons-*-b.pk3")) {
        Copy-Item -LiteralPath $p.FullName -Destination (PathOf ("mods\" + $p.Name)) -Force
    }
    Say "Doom and Heretic guns: Game Or Die's own, held in your hands (Doom 64 and PSX Doom too)"
}

# ---------------------------------------------------------------------------
# Brutal Doom 64's status bar draws its stamina number over the health number
# (both large, at x 25 and x 45, in its "normal" bar). Made here from the
# player's own copy: its SBARINFO with that one line moved small under the
# health, loaded after the mod in Modern style. Nothing of the mod's is shipped.
# ---------------------------------------------------------------------------
$bd64 = PathOf "mods\bd64gamev2.pk3"
$bd64Fix = PathOf "mods\doomvr-bd64-statusbar.pk3"
if ((Test-Path -LiteralPath $bd64) -and -not (Test-Path -LiteralPath $bd64Fix)) {
    try {
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $zin = [IO.Compression.ZipFile]::OpenRead($bd64)
        try {
            $entry = $zin.Entries | Where-Object { $_.FullName -ieq "SBARINFO.txt" } | Select-Object -First 1
            $sbar = if ($entry) { (New-Object IO.StreamReader($entry.Open())).ReadToEnd() } else { $null }
        } finally { $zin.Dispose() }
        $was = 'drawnumber 3, HUDFONT_DOOM, untranslated, Ammo("Stamina"), 25, 177;'
        if ($sbar -and $sbar.Contains($was)) {
            $sbar = $sbar.Replace($was, 'drawnumber 3, SMALLFONT, untranslated, Ammo("Stamina"), 45, 193;')
            $zout = [IO.Compression.ZipFile]::Open($bd64Fix, "Create")
            try {
                $w = New-Object IO.StreamWriter(($zout.CreateEntry("SBARINFO.txt")).Open())
                $w.Write($sbar); $w.Close()
            } finally { $zout.Dispose() }
            Say "Brutal Doom 64: its stamina number moved off the health number"
        }
    } catch { Say ("Brutal Doom 64 status bar: left as it is - " + $_.Exception.Message) }
}

# ---------------------------------------------------------------------------
# DOOM 64 CE's IWAD: the player's DOOM64.WAD with CE's BPS patch applied.
# bps.ps1 checks the source, the result and the patch against the CRCs the
# patch carries, so a different release of the WAD is refused, not mangled.
# ---------------------------------------------------------------------------
if (-not $LinkWads -and -not (Has "doomce\DOOM64.IWAD")) {
    $d64 = PathOf "wads\DOOM64.WAD"
    $bps = PathOf "doomce\DOOM64.bps"
    if ((Test-Path -LiteralPath $d64) -and (Test-Path -LiteralPath $bps)) {
        . (Join-Path $PSScriptRoot "bps.ps1")
        $why = Apply-Bps $d64 $bps (PathOf "doomce\DOOM64.IWAD")
        if ($why) { Say ("Doom 64: could not patch DOOM64.WAD - " + $why) }
        else { Say "Doom 64: patched your DOOM64.WAD into doomce\DOOM64.IWAD for DOOM 64 CE (checked)" }
    } elseif (Test-Path -LiteralPath $bps) {
        Say "Doom 64: DOOM 64 CE is here, but no DOOM64.WAD was found (Steam, GOG or Epic's DOOM 64)."
    }
}

# ---------------------------------------------------------------------------
# DOOM 64 CE's Lost Levels: the 2020 re-release's bonus episode, which CE
# ships only as seven BPS patches (patcher\LOST01-07.bps in its zips) for
# the player's own DOOM64.WAD - its MAP34 to MAP40, each a WAD inside the
# WAD. Patched here into maps/LOST01-07.wad in DOOM64.CE.Maps.LostLevels.pk3,
# the name CE's IWADINFO loads. The patches come from the CE zip still in
# Downloads when they are not already beside the game.
# ---------------------------------------------------------------------------
function Get-WadLumpBytes([string]$wadPath, [string]$lumpName) {
    $fs = [IO.File]::OpenRead($wadPath)
    try {
        $br = New-Object IO.BinaryReader($fs)
        [void]$br.ReadBytes(4)
        $count = $br.ReadInt32(); $dirOfs = $br.ReadInt32()
        $fs.Position = $dirOfs
        $dir = $br.ReadBytes(16 * $count)
        for ($i = 0; $i -lt $count; $i++) {
            $nm = ([Text.Encoding]::ASCII.GetString($dir, $i * 16 + 8, 8)).TrimEnd([char]0)
            if ($nm -ne $lumpName) { continue }
            $fs.Position = [BitConverter]::ToInt32($dir, $i * 16)
            return $br.ReadBytes([BitConverter]::ToInt32($dir, $i * 16 + 4))
        }
    } finally { $fs.Close() }
    return $null
}

if (-not $LinkWads -and (Has "doomce\DOOM64.CE.ipk3") -and -not (Has "doomce\DOOM64.CE.Maps.LostLevels.pk3") -and (Has "wads\DOOM64.WAD")) {
    $ceDir = PathOf "doomce"
    $lostPatches = 1..7 | ForEach-Object { Join-Path $ceDir ("LOST0{0}.bps" -f $_) }
    if (@($lostPatches | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -gt 0) {
        $ceZip = Get-ChildItem -LiteralPath $Downloads -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -like "DOOM64.CE-*.zip" -or $_.Name -like "DOOM.CE-Lite-*.zip" } |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($ceZip) { [void](Expand-Wanted $ceZip.FullName @("LOST0?.bps") $ceDir) }
    }
    if (@($lostPatches | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -eq 0) {
        . (Join-Path $PSScriptRoot "bps.ps1")
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $lostPk3 = PathOf "doomce\DOOM64.CE.Maps.LostLevels.pk3"
        $tmpDir = Join-Path $ceDir "lostlevels.tmp"
        New-Item -ItemType Directory -Force $tmpDir | Out-Null
        $failed = ""
        try {
            for ($n = 1; $n -le 7 -and -not $failed; $n++) {
                $lump = Get-WadLumpBytes (PathOf "wads\DOOM64.WAD") ("MAP{0}" -f (33 + $n))
                if (-not $lump) { $failed = "your DOOM64.WAD has no MAP$(33 + $n) - it is not the 2020 re-release"; break }
                $src = Join-Path $tmpDir "src.wad"
                [IO.File]::WriteAllBytes($src, $lump)
                $why = Apply-Bps $src (Join-Path $ceDir ("LOST0{0}.bps" -f $n)) (Join-Path $tmpDir ("LOST0{0}.wad" -f $n))
                if ($why) { $failed = "LOST0$n - $why" }
            }
            if (-not $failed) {
                $new = $lostPk3 + ".new"
                if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Force }
                $zip = [IO.Compression.ZipFile]::Open($new, "Create")
                try {
                    for ($n = 1; $n -le 7; $n++) {
                        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $tmpDir ("LOST0{0}.wad" -f $n)), ("maps/LOST0{0}.wad" -f $n))
                    }
                } finally { $zip.Dispose() }
                Move-Item -LiteralPath $new -Destination $lostPk3 -Force
                Say "Doom 64: made the Lost Levels (the 2020 bonus episode) from your DOOM64.WAD for DOOM 64 CE (checked)"
            } else {
                Say ("Doom 64: could not make the Lost Levels - " + $failed)
            }
        } finally { Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------------------------------
# Remastered music. The re-release keeps its soundtrack in wads of its own,
# under names the game never asks for: the engine wants MUS_E1M1 and the wad
# offers H_DARK. The IWAD carries the mapping in a TRAKINFO lump - a hash per
# entry, useless to us, but with the original lump's name in a comment above
# it. That is enough to write the engine's own $musicalias, which is how it
# already points Doom's episode 4 at episode 3's music. Written here, from the
# player's own files, every run.
# ---------------------------------------------------------------------------
function Get-WadLump([string]$wadPath, [string]$lumpName) {
    if (-not (Test-Path -LiteralPath $wadPath)) { return $null }
    $fs = [IO.File]::OpenRead($wadPath)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $magic = [Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
        if ($magic -ne "IWAD" -and $magic -ne "PWAD") { return $null }
        $count = $br.ReadInt32(); $dirOfs = $br.ReadInt32()
        $fs.Position = $dirOfs
        for ($i = 0; $i -lt $count; $i++) {
            $pos = $br.ReadInt32(); $size = $br.ReadInt32()
            $nm = ([Text.Encoding]::ASCII.GetString($br.ReadBytes(8))).TrimEnd([char]0)
            if ($nm -eq $lumpName) {
                $fs.Position = $pos
                return [Text.Encoding]::UTF8.GetString($br.ReadBytes($size))
            }
        }
    } finally { $fs.Close() }
    return $null
}

function Make-MusicPatch([string]$iwadPath, [string]$outPk3, [string]$trackKind) {
    $trak = Get-WadLump $iwadPath "TRAKINFO"
    if (-not $trak) { return 0 }
    $rx = [regex]'(?s)//\s*(\w+)\s*\r?\n\s*"[0-9a-fA-F]+"\s*:\s*\{(.*?)\}'
    $lines = @(
        ("// Generated by DoomVR's SETUP.bat from " + [IO.Path]::GetFileName($iwadPath)),
        ("// Points the game's own music lumps at the re-release's " + $trackKind + " tracks."),
        ("// The matching *_mus_*.wad must be loaded too; this only maps names."),
        ("")
    )
    $n = 0
    foreach ($m in $rx.Matches($trak)) {
        $t = [regex]::Match($m.Groups[2].Value, ('"' + $trackKind + '"\s*:\s*"([^"]+)"'))
        if (-not $t.Success) { continue }
        $lines += ("`$musicalias {0,-12} {1}" -f $m.Groups[1].Value, $t.Groups[1].Value)
        $n++
    }
    if ($n -eq 0) { return 0 }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $outPk3) { Remove-Item -LiteralPath $outPk3 -Force }
    $z = [IO.Compression.ZipFile]::Open($outPk3, "Create")
    try {
        $e = $z.CreateEntry("sndinfo.txt")
        $w = New-Object IO.StreamWriter($e.Open())
        $w.Write(($lines -join "`r`n") + "`r`n")
        $w.Close()
    } finally { $z.Dispose() }
    return $n
}

if (-not $LinkWads) {
    foreach ($k in $musicSets.Keys) {
        $m = $musicSets[$k]
        if (-not ((Has $m.iwad) -and (Has $m.wad))) { continue }
        $made = Make-MusicPatch (PathOf $m.iwad) (PathOf $m.pk3) "Remixed"
        if ($made -gt 0) { Say ("Wrote {0}: {1} tracks mapped to the remastered soundtrack" -f $m.pk3, $made) }
    }
}

# ---------------------------------------------------------------------------
# Launchers, for every game whose files are all there.
# ---------------------------------------------------------------------------
$rel = if ($LinkWads) { $LinkWads.TrimEnd('\') } else { "wads" }
# How a launcher names that folder: beside the install, or absolute.
$ref = if ([System.IO.Path]::IsPathRooted($rel)) { $rel } else { "%~dp0..\" + $rel }
function RefOf([string]$f) {
    if ($f.StartsWith("wads\")) { return ($ref + "\" + $f.Substring(5)) }
    return ("%~dp0..\" + $f)
}

# Every entry. The voxels, gore, sounds and remastered music are not part of
# a game's own line any more: they are its Modern VR style (ModernFor).
$entries = @()
foreach ($g0 in $games) {
    # Optional files (a mod's second half, its music) join the line only when
    # they are there, so a launcher never names a file that is not.
    $g = @{ name = $g0.name; iwad = $g0.iwad; files = @($g0.files | Where-Object { $_ }); needs = $g0.needs; music = $g0.music; dirs = $g0.dirs; args = $g0.args; own = @($g0.files | Where-Object { $_ }) }
    foreach ($o in @($g0.optional | Where-Object { $_ })) { if (Has $o) { $g.files += $o } }
    $entries += $g
}

# The group each game sits under in Switch Game (doomvr_switchgame.cpp keeps
# the groups in this order).
$groupOf = @{}
foreach ($n in "Freedoom Phase 1", "Freedoom Phase 2", "FreeDM", "Blasphemer", "Chex Quest 3", "HacX", "Adventures of Square") { $groupOf[$n] = "Free games" }
foreach ($n in "Project Osiris", "Return of the Triad", "Castlevania - Simon's Destiny", "Brutal Wolfenstein", "Blood - ZBloody Hell", "Aliens - Eradication", "Ashes 2063", "Duke Nukem 3D TC") { $groupOf[$n] = "Total conversions" }
foreach ($n in "Project Brutality", "MeatGrinder", "Beautiful Doom") { $groupOf[$n] = "Gameplay mods" }
# Each Brutal Doom with the game it plays; Brutal Doom 64 is with Doom 64 below.
$groupOf["Brutal Doom"] = "Doom"
$groupOf["Brutal Doom II"] = "Doom II"
foreach ($n in "Doom 64", "PSX Doom", "PSX Final Doom", "Brutal Doom 64", "Doom 64 Retribution") { $groupOf[$n] = "Doom 64 and PlayStation" }
function GroupOf($g) {
    if ($groupOf.ContainsKey($g.name)) { return $groupOf[$g.name] }
    $i = ([string]$g.iwad).ToLower()
    # The game's own files - not the voxel and gore packs added on top.
    $own = if ($g.ContainsKey("own")) { $g.own } else { $g.files }
    $modded = @($own | Where-Object { $_ -like "mods\*" -and $_ -notlike "mods\hh_patch_*" -and $_ -notlike "mods\music_*" }).Count -gt 0
    if ($modded) { return "Megawads" }
    if ($i -match 'heretic') { return "Heretic" }
    if ($i -match 'hexen|hexdd') { return "Hexen" }
    if ($i -match 'strife|sve') { return "Strife" }
    if ($i -match 'tnt|plutonia') { return "Final Doom" }
    if ($i -match 'doom2') { return "Doom II" }
    if ($i -match 'doom\.wad') { return "Doom" }
    return "Other"
}

# ---------------------------------------------------------------------------
# What a game is (its tags, read from its files) and the add-ons with their
# rules - used by the launchers' Modern VR style and by Build Your Own Game.
# The tags are described with the catalogue below.
# ---------------------------------------------------------------------------
# The sprite names of the games' own monsters: Doom, Heretic, Hexen.
$monsterSprites = @{}
foreach ($m in ("POSS SPOS CPOS TROO SARG HEAD SKUL BOSS BOS2 PAIN SKEL FATT BSPI CYBR SPID VILE SSWV KEEN " +
                "IMPX KNIG WZRD MUMM BEAS CLNK SNKE MNTR SRCR SOR2 " +
                "ETTN CENT DEMN DEM2 SERP WRTH BISH FDMN ICEY SORC KORX DRAG").Split(" ")) { $monsterSprites[$m] = 1 }
# Doom's weapons as held (first person; pickup pictures alone are not enough -
# Legacy of Rust adds pickup frames and keeps the guns): a game that redraws
# them has its own guns, and a 3D Doom gun made from the original pictures
# would not be the gun it shows (Voxel Doom's in hand, or ours made at setup).
$weaponSprites = @{}
foreach ($w in "PISG SHTG SHT2 CHGG MISG PLSG BFGG SAWG".Split(" ")) { $weaponSprites[$w] = 1 }
function ContentTags([string[]]$paths) {
    $t = @{}
    $mons = @{}
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    foreach ($p in $paths) {
        if (-not $p -or -not (Test-Path -LiteralPath $p)) { continue }
        $names = New-Object System.Collections.Generic.List[string]
        $isWad = $false
        $fs = [IO.File]::OpenRead($p)
        try {
            $br = New-Object IO.BinaryReader($fs)
            $magic = [Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
            if ($magic -eq "IWAD" -or $magic -eq "PWAD") {
                $isWad = $true
                $count = $br.ReadInt32(); $dirOfs = $br.ReadInt32()
                $fs.Position = $dirOfs
                $dir = $br.ReadBytes(16 * $count)
                for ($i = 0; $i -lt $count; $i++) { $names.Add(([Text.Encoding]::ASCII.GetString($dir, $i * 16 + 8, 8)).TrimEnd([char]0).ToUpper()) }
            }
        } finally { $fs.Close() }
        if ($isWad) {
            $inSprites = $false
            foreach ($n in $names) {
                if ($n -in "S_START", "SS_START") { $inSprites = $true; continue }
                if ($n -in "S_END", "SS_END") { $inSprites = $false; continue }
                if ($inSprites -and $n.Length -ge 4 -and $monsterSprites.ContainsKey($n.Substring(0, 4))) { $mons[$n.Substring(0, 4)] = 1 }
                if ($inSprites -and $n.Length -ge 4 -and $weaponSprites.ContainsKey($n.Substring(0, 4))) { $t["ownweapons"] = 1 }
                if ($n -in "DEHACKED", "DECORATE", "ZSCRIPT") { $t["ownactors"] = 1 }
                if ($n -in "TEXTURE1", "TEXTURE2", "TEXTURES", "P_START", "PP_START", "TX_START") { $t["owntextures"] = 1 }
                if ($n -like "D_*" -or $n -eq "MUSINFO") { $t["ownmusic"] = 1 }
                if ($n -like "DS*" -or $n -eq "SNDINFO") { $t["ownsounds"] = 1 }
            }
        } elseif (Is-Zip $p) {
            $z = [IO.Compression.ZipFile]::OpenRead($p)
            try {
                foreach ($e in $z.Entries) {
                    $n = $e.FullName.ToUpper()
                    if ($n -match '^(DECORATE|ZSCRIPT|DEHACKED)') { $t["ownactors"] = 1 }
                    if ($n -match '^SPRITES/(?:.*/)?([A-Z0-9]{4})[^/]*$' -and $monsterSprites.ContainsKey($Matches[1])) { $mons[$Matches[1]] = 1 }
                    if ($n -match '^SPRITES/(?:.*/)?([A-Z0-9]{4})[^/]*$' -and $weaponSprites.ContainsKey($Matches[1])) { $t["ownweapons"] = 1 }
                    if ($n -match '^(TEXTURES|TEXTURE1|TEXTURE2)(\.|$)|^(TEXTURES|PATCHES|FLATS)/') { $t["owntextures"] = 1 }
                    if ($n -match '^MUSIC/') { $t["ownmusic"] = 1 }
                    if ($n -match '^SOUNDS/') { $t["ownsounds"] = 1 }
                }
            } finally { $z.Dispose() }
        }
    }
    if ($mons.Count -ge 3) { $t["ownmonsters"] = 1 } elseif ($mons.Count -gt 0) { $t["somemonsters"] = 1 }
    return @($t.Keys | Sort-Object)
}

function IdOf([string]$kind, [string]$label) {
    return $kind + "-" + (($label.ToLower() -replace '[^a-z0-9]+', '-').Trim('-'))
}

# What a game is, for the rules: which game (doom1, doom2, heretic...), the
# 2024/2025 re-release (kex...), and maps of its own beyond the game's
# (No Rest for the Living, SIGIL, Faith Renewed...).
function BaseTags($g) {
    $i = ([string]$g.iwad).ToLower()
    $kex = $i.StartsWith("kex\")
    $t = @()
    if ($i -match '\\doom\.wad$')          { $t += "doom", "doom1"; if ($kex) { $t += "kexdoom1" } }
    elseif ($i -match '\\doom2\.wad$')     { $t += "doom", "doom2"; if ($kex) { $t += "kexdoom2" } }
    elseif ($i -match '\\(tnt|plutonia)\.wad$') { $t += "doom", "finaldoom" }
    elseif ($i -match '\\freedoom1\.wad$') { $t += "doom", "doom1", "ownmonsters", "ownactors", "owntextures", "ownmusic", "ownsounds", "ownweapons" }
    elseif ($i -match '\\freedoom2\.wad$') { $t += "doom", "doom2", "ownmonsters", "ownactors", "owntextures", "ownmusic", "ownsounds", "ownweapons" }
    elseif ($i -match '\\heretic\.wad$')   { $t += "heretic"; if ($kex) { $t += "kexheretic" } }
    elseif ($i -match '\\hexen\.wad$')     { $t += "hexen"; if ($kex) { $t += "kexhexen" } }
    elseif ($i -match '\\hexdd\.wad$')     { $t += "hexen", "hasmaps" }
    else { return $null }
    $extra = @($g.own | Where-Object { $_ -and $_ -notlike "mods\hh_patch_*" -and $_ -notlike "kex\*_ex.wad" -and $_ -notlike "mods\music_*" })
    if ($extra.Count -gt 0) { $t += "hasmaps"; $t += ContentTags @($extra | ForEach-Object { PathOf $_ }) }
    return @($t | Select-Object -Unique)
}

# The add-ons that are not maps or gameplay mods, and their rules.
#   Voxel Doom and the look packs swap the game's own monsters and things for
#   new art by sprite name, so a gameplay mod (its own monsters) or maps that
#   redraw the monsters leave them showing the wrong thing.
#   The voxel gore is an add-on to NashGore, and stops the game without it.
#   IDKFA is Doom's (the first game's) music only - it has no Doom II tracks.
$addOns = @(
    @{ kind = "looks";  label = "Voxel Doom";              files = @("mods\VoxelDoom_v2.4.pk3");            has = "voxels voxeldoom"; needs = "doom"; clashes = "gameplay ownmonsters"; warns = "somemonsters" },
    # Game Or Die Hands' held weapons, made at setup from the player's games
    # (the gdh_setup step above): Doom's own eight, animated - after Voxel
    # Doom, whose monsters and floor pickups stay - then everything else
    # (every class they name exists in every game), then RHVOX's and RZXVOX's
    # own weapon voxels, each straight after its mod, overriding the made
    # ones. A gameplay mod, or a game that draws its own guns, brings its own.
    @{ kind = "looks";  label = "Doom weapons in hand (made from your game)"; files = @("mods\doomvr-voxel-weapons-doom.pk3"); has = ""; needs = "doom"; clashes = "gameplay ownweapons" },
    @{ kind = "looks";  label = "Weapons in hand (made from your games)";     files = @("mods\doomvr-voxel-weapons-extruded.pk3");     has = ""; needs = "";     clashes = "gameplay" },
    # Game Or Die's own Doom guns (setup\gdh, our models): Doom's eight and
    # Legacy of Rust's two, each held by our hands in its own fitted grip -
    # after the made packs, so these are the guns in hand.
    # With them, the same guns on the Doom slots Legacy of Rust gives its own
    # two weapons (tools/id24-slot-guns.py; nothing changes in other games).
    @{ kind = "looks";  label = "Game Or Die's Doom guns";  files = @("mods\doomvr-weapons-doom-b.pk3", "mods\doomvr-weapons-id24slots-b.pk3");   has = ""; needs = "doom"; clashes = "gameplay ownweapons" },
    # Game Or Die's own Heretic guns (setup\gdh, our models), held by the
    # Doom hands in their fitted grips - after the made pack. Heretic only:
    # its MODELDEF names Heretic's weapon classes.
    @{ kind = "looks";  label = "Game Or Die's Heretic guns"; files = @("mods\doomvr-weapons-heretic-b.pk3");   has = ""; needs = "heretic"; clashes = "gameplay ownweapons" },
    # The re-release's ninth, the Tempest Wand: its own pack, as its classes
    # are only in the 2025 games (hh_patch_common.pk3) - a MODELDEF naming a
    # class that is not there stops the game. After the eight.
    @{ kind = "looks";  label = "Game Or Die's Tempest Wand"; files = @("mods\doomvr-weapons-heretic2025-b.pk3"); has = ""; needs = "kexheretic"; clashes = "gameplay ownweapons" },
    @{ kind = "looks";  label = "NashGore";                files = @("mods\nashgore.pk3");                  has = "nashgore"; needs = "doom";         clashes = "gameplay" },
    @{ kind = "looks";  label = "NashGore voxel gore";     files = @("mods\nashgore_voxels_official.pk3");  has = "";         needs = "doom nashgore"; clashes = "gameplay" },
    @{ kind = "looks";  label = "Neural Upscale textures"; files = @("mods\NeuralUpscale2x_v1.0.pk3");      has = "";         needs = "doom";         clashes = "gameplay ownmonsters owntextures"; warns = "somemonsters" },
    @{ kind = "looks";  label = "Heretic voxels";          files = @("mods\RHVOX-0.11.pk3");                has = "voxels rhvox";  needs = "heretic";      clashes = "ownmonsters"; warns = "somemonsters" },
    # (RHVOX's own crossbow voxel - doomvr-voxel-weapons-rhvox.pk3 - is no longer loaded: our made
    # crossbow is animated, the one held. The file is still made.)
    @{ kind = "looks";  label = "Hexen voxels";            files = @("mods\RZXVOX-0.12.pk3");               has = "voxels rzxvox"; needs = "hexen";        clashes = "ownmonsters"; warns = "somemonsters" },
    @{ kind = "looks";  label = "Hexen voxel weapons in hand";   files = @("mods\doomvr-voxel-weapons-rzxvox.pk3"); has = ""; needs = "rzxvox"; clashes = "gameplay" },
    @{ kind = "sounds"; label = "Sounds Remastered";       files = @("mods\pk_doom_sfx_20120224.wad");      has = "";         needs = "doom";         warns = "gameplay ownsounds" },
    @{ kind = "music";  label = "IDKFA soundtrack";        files = @("mods\IDKFAv2.wad");                   has = "";         needs = "doom1";        warns = "ownmusic" },
    @{ kind = "music";  label = "Heretic remastered soundtrack"; files = @("kex\heretic_mus_remix.wad", "mods\music_heretic_remix.pk3"); has = ""; needs = "kexheretic" },
    @{ kind = "music";  label = "Hexen remastered soundtrack";   files = @("kex\hexen_mus_remix.wad", "mods\music_hexen_remix.pk3");     has = ""; needs = "kexhexen" }
)


# The Modern VR style: every add-on that fits this game cleanly - its needs
# met, nothing it clashes with, nothing it only mostly works with - in the
# table's order (looks, then sounds, then music). The launcher names them with
# -modern, and the engine loads them only when vr_style is Modern (the
# default); Classic is the game as it was made. Total conversions, the CE
# games and Strife bring their own look and get none.
function ModernFor($g) {
    $grp = GroupOf $g
    # Strife and Chex keep their own look, but get the held weapons made from
    # them at setup - not a look, just something for our hands to hold.
    $made = "mods\doomvr-voxel-weapons-extruded.pk3"
    if (($grp -eq "Strife" -or ([string]$g.iwad) -match 'chex') -and -not (@($g.files) -match 'mods\\')) {
        if (Has $made) { return @($made) } else { return @() }
    }
    # DOOM CE's games and Doom 64: Retribution have weapons of their own that
    # take the Doom ones' places; our Doom guns reach them through a pack
    # naming those weapons (made above). Their own look otherwise.
    $guns = "mods\doomvr-weapons-doom-b.pk3"
    $onto = if ([string]$g.iwad -match 'DOOM64\.CE') { "mods\doomvr-weapons-doom64ce-b.pk3" }
            elseif ([string]$g.iwad -match 'PSX(FINAL)?DOOM\.CE') { "mods\doomvr-weapons-psxce-b.pk3" }
            elseif (@($g.files) -match 'D64RTR') { "mods\doomvr-weapons-d64rtr-b.pk3" }
            # The Brutal family's weapons are their own and replace nothing:
            # their packs also name the Doom weapon each one stands for.
            elseif (@($g.files) -match 'brutalv21\.pk3') { "mods\doomvr-weapons-brutaldoom-b.pk3" }
            elseif (@($g.files) -match 'PB-0_3_1-alpha\.pk3') { "mods\doomvr-weapons-projectbrutality-b.pk3" }
            elseif (@($g.files) -match 'meatgrinderV2C\.pk3') { "mods\doomvr-weapons-meatgrinder-b.pk3" }
            elseif (@($g.files) -match 'bd64gamev2\.pk3') { "mods\doomvr-weapons-brutaldoom64-b.pk3" }
            else { $null }
    if ($onto) {
        $out = if ((Has $guns) -and (Has $onto)) { @($guns, $onto) } else { @() }
        if ($onto -like "*brutaldoom64*" -and (Has "mods\doomvr-bd64-statusbar.pk3")) { $out += "mods\doomvr-bd64-statusbar.pk3" }
        return $out
    }
    if ($grp -in "Total conversions", "Doom 64 and PlayStation", "Strife", "Other") { return @() }
    $have = BaseTags $g
    if ($null -eq $have) { return @() }
    $have = @($have)
    if ($grp -eq "Gameplay mods") { $have += "gameplay" }
    $out = @()
    foreach ($a in $addOns) {
        if (@($a.files | Where-Object { -not (Has $_) }).Count -gt 0) { continue }
        $ok = $true
        foreach ($n in @(([string]$a.needs) -split ' ' | Where-Object { $_ })) {
            if (-not @($n -split '\|' | Where-Object { $have -contains $_ })) { $ok = $false }
        }
        foreach ($t in @(([string]$a.clashes) -split ' ' | Where-Object { $_ }) + @(([string]$a.warns) -split ' ' | Where-Object { $_ })) {
            if ($have -contains $t) { $ok = $false }
        }
        if (-not $ok) { continue }
        $out += $a.files
        $have += @(([string]$a.has) -split ' ' | Where-Object { $_ })
    }
    return $out
}

# A launcher. Build Your Own Game writes its own from the same text, so it
# goes into the catalogue as a template. It ends "-file %*": a mod dropped on
# the launcher arrives as %*, and straight after a + command it would be taken
# as that command's argument and never loaded.
function LauncherText([string]$name, [string]$group, [string]$launch, [string]$by) {
    return @"
@echo off
rem DoomVR - $name. $by
rem DoomVR-group: $group
pushd "%~dp0.."
if exist "log-doomvr.txt" move /y "log-doomvr.txt" "log-doomvr.prev.txt" >nul
if exist "log-doomvr-shutdown.txt" move /y "log-doomvr-shutdown.txt" "log-doomvr-shutdown.prev.txt" >nul
.\uzdoom.exe $launch -config "%~dp0..\doomvr.ini" +vid_preferbackend 0 +vid_fullscreen 1 +vr_mode 20 +vr_current_game "$name" +logfile doomvr -file %*
"@
}

$ldir = Join-Path $Install "launchers"
New-Item -ItemType Directory -Force $ldir | Out-Null
$written = @(); $names = @(); $writtenEntries = @()
foreach ($g in $entries) {
    if ($names -notcontains $g.name) { $names += $g.name }
    if ($written -contains $g.name) { continue }
    $all = @($g.iwad) + @($g.files) + @($g.needs) | Where-Object { $_ }
    if (@($all | Where-Object { -not (Has $_) }).Count -gt 0) { continue }
    $launch = "-iwad `"" + (RefOf $g.iwad) + "`""
    foreach ($f in @($g.files) | Where-Object { $_ }) { $launch += " -file `"" + (RefOf $f) + "`"" }
    foreach ($d in @($g.dirs) | Where-Object { $_ }) { $launch += " -iwaddir `"" + (RefOf ($d + "\")).TrimEnd('\') + "`"" }
    $g.modern = @(ModernFor $g)
    foreach ($f in $g.modern) { $launch += " -modern `"" + (RefOf $f) + "`"" }
    if ($g.args) { $launch += " " + $g.args }
    # For Build Your Own Game: the game with only its own files - not its
    # Modern packs, which the player picks there.
    $own = "-iwad `"" + (RefOf $g.iwad) + "`""
    foreach ($f in @($g.own) | Where-Object { $_ }) { $own += " -file `"" + (RefOf $f) + "`"" }
    foreach ($d in @($g.dirs) | Where-Object { $_ }) { $own += " -iwaddir `"" + (RefOf ($d + "\")).TrimEnd('\') + "`"" }
    if ($g.args) { $own += " " + $g.args }
    $g.ownLaunch = $own
    $bat = LauncherText $g.name (GroupOf $g) $launch "Written by SETUP.bat; running it again rewrites this."
    [System.IO.File]::WriteAllText((Join-Path $ldir ($g.name + ".bat")), $bat.Replace("`r`n", "`n").Replace("`n", "`r`n"), [System.Text.Encoding]::ASCII)
    $written += $g.name
    $writtenEntries += $g
}
$missing = @($names | Where-Object { $written -notcontains $_ })
# Launchers this version no longer writes: the "- remastered music" twins,
# which the Modern VR style replaces. Only ones setup wrote itself.
Get-ChildItem -LiteralPath $ldir -Filter "* - remastered music.bat" -File -ErrorAction SilentlyContinue |
    Where-Object { (Get-Content -LiteralPath $_.FullName -TotalCount 2) -match "Written by SETUP.bat" } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName; Say ("Retired launchers\" + $_.Name + " - its music is now the Modern VR style") }
# ...and the covers setup made for them (boxart\auto is setup's own; the
# player's covers in boxart\ are never touched).
Get-ChildItem -LiteralPath (Join-Path $Install "boxart\auto") -Filter "* - remastered music.png" -File -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName }
$nModern = @($writtenEntries | Where-Object { $_.modern.Count -gt 0 }).Count

# DoomVR.bat starts Doom - the 2024 re-release (Nightdive's, widescreen) when it is there, else the original - then Doom II, or the first there is -
# with the game picker up over it (Switch Game, "Continue: <the game played last>" first).
$start = ""
foreach ($d in "Doom (2024 re-release)", "Doom", "Doom II (2024 re-release)", "Doom II") { if (-not $start -and $written -contains $d) { $start = $d } }
if (-not $start -and $written.Count -gt 0) { $start = $written[0] }
if ($start) {
    $top = @"
@echo off
rem DoomVR. Opens the game picker (over $start); Switch Game (main menu, or Options) has every game.
call "%~dp0launchers\$start.bat" %* +vr_open_picker 1
"@
    [System.IO.File]::WriteAllText((Join-Path $Install "DoomVR.bat"), $top.Replace("`r`n", "`n").Replace("`n", "`r`n"), [System.Text.Encoding]::ASCII)
    # The same, on the monitor: no headset, for trying a mod or showing
    # someone. Every launcher passes on what follows it, so this vr_mode wins.
    # A file dropped on it comes first: after a + command it would be taken as
    # that command's argument and never loaded.
    $flat = @"
@echo off
rem DoomVR on the monitor, no headset. Starts $start; Switch Game has the rest.
call "%~dp0launchers\$start.bat" %* +vr_mode 0
"@
    [System.IO.File]::WriteAllText((Join-Path $Install "DoomVR - flat screen.bat"), $flat.Replace("`r`n", "`n").Replace("`n", "`r`n"), [System.Text.Encoding]::ASCII)
}

# ---------------------------------------------------------------------------
# Build Your Own Game's catalogue, launchers\_build.txt: every game here that
# can be built on, and every add-on, with what each needs, what it clashes
# with and what it only mostly works with. The page in the game applies the
# rules; src\common\menu\doomvr_build.cpp documents the format. What a
# megawad or a game's extra maps carry of their own - monsters, textures,
# music, sounds - is read from their lumps, not written down by hand.
#
#   ownmonsters   redraws three or more of the games' own monsters (Sunlust,
#                 Back to Saturn X, Valiant): a voxel pack would hide its art
#   somemonsters  redraws one or two (Lunatic's and UAC Ultra's Wolfenstein
#                 SS): a voxel pack is worth a warning
#   ownactors     DEHACKED, DECORATE or ZSCRIPT - changes how things behave,
#                 which a gameplay mod's own monsters replace
#   owntextures, ownmusic, ownsounds
# ---------------------------------------------------------------------------
if ($writtenEntries.Count -gt 0) {
    function CatLine([string]$kind, [string]$label, $has, $needs, $clashes, $warns, [string]$launch) {
        $f = { param($x) $v = (@($x | Where-Object { $_ }) -join " ").Trim(); if ($v) { $v } else { "-" } }
        return ($kind, (IdOf $kind $label), $label, (& $f $has), (& $f $needs), (& $f $clashes), (& $f $warns), $launch.Trim()) -join "`t"
    }
    function FilesArg($files) { return ((@($files | Where-Object { $_ -and (Has $_) }) | ForEach-Object { "-file `"" + (RefOf $_) + "`"" }) -join " ") }

    $cat = @("# Build Your Own Game's catalogue. Written by SETUP.bat; running it again rewrites this.",
             "# kind`tid`tlabel`thas`tneeds`tclashes`twarns`targs")
    foreach ($line in (LauncherText "{NAME}" "{GROUP}" "{ARGS}" "Made in Build Your Own Game; delete this file to take it off the list.").Replace("`r`n", "`n").TrimEnd("`n").Split("`n")) { $cat += "template`t" + $line }
    $nBase = 0; $nAdd = 0
    foreach ($g in $writtenEntries) {
        if ($g.name -like "* - remastered music") { continue }
        $grp = GroupOf $g
        if ($grp -notin "Doom", "Doom II", "Final Doom", "Heretic", "Hexen" -and $g.name -notin "Freedoom Phase 1", "Freedoom Phase 2") { continue }
        $tags = BaseTags $g
        if ($null -eq $tags) { continue }
        $cat += CatLine "base" $g.name $tags @() @() @() $g.ownLaunch
        $nBase++
    }
    # Maps: the megawads - Doom II's, and one for Doom.
    $mapSets = @($onDoom2 | Where-Object { -not $groupOf.ContainsKey($_.name) } | ForEach-Object { @{ label = $_.name; files = $_.files; needs = "doom2" } }) +
               @(@{ label = "Doom the Way id Did"; files = @("mods\dtwid.wad"); needs = "doom1" })
    foreach ($m in $mapSets) {
        if (-not (Has $m.files[0])) { continue }
        $tags = @("maps") + (ContentTags @($m.files | Where-Object { Has $_ } | ForEach-Object { PathOf $_ }))
        $cat += CatLine "maps" $m.label $tags $m.needs "hasmaps" @() (FilesArg $m.files)
        $nAdd++
    }
    foreach ($m in @($onDoom2 | Where-Object { $groupOf[$_.name] -eq "Gameplay mods" })) {
        if (-not (Has $m.files[0])) { continue }
        $cat += CatLine "gameplay" $m.name "gameplay" "doom" @() ("ownmonsters", "ownactors") (FilesArg $m.files)
        $nAdd++
    }
    foreach ($a in $addOns) {
        if (@($a.files | Where-Object { -not (Has $_) }).Count -gt 0) { continue }
        $cat += CatLine $a.kind $a.label ($a.has -split ' ') ($a.needs -split ' ') ($a.clashes -split ' ') ($a.warns -split ' ') (FilesArg $a.files)
        $nAdd++
    }
    [IO.File]::WriteAllText((Join-Path $ldir "_build.txt"), (($cat -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($false)))
    Say ("Build Your Own Game: {0} games to build on, {1} add-ons (launchers\_build.txt)" -f $nBase, $nAdd)
}

# ---------------------------------------------------------------------------
# The art. boxart\<game>.png or .jpg (600x900) and boxart\graphics\vrsplash
# .png or .jpg (1920x1080) are the player's own and always win; then Game Or
# Die's own covers, shipped beside this script in boxart\; for the rest a
# cover is made from the game's own title screen into boxart\auto\, once,
# and a splash from all of them. Everything goes into boxart.pk3.
# ---------------------------------------------------------------------------
if (-not $LinkWads -and $writtenEntries.Count -gt 0) {
    try {
        . (Join-Path $PSScriptRoot "art.ps1")
        $artDir = Join-Path $Install "boxart"
        $autoDir = Join-Path $artDir "auto"
        New-Item -ItemType Directory -Force (Join-Path $artDir "graphics"), $autoDir | Out-Null
        $readme = Join-Path $artDir "PUT YOUR COVERS HERE.txt"
        if (-not (Test-Path -LiteralPath $readme)) {
            [IO.File]::WriteAllText($readme, "Covers for Switch Game, and the startup splash - your own art wins over the ones setup makes.`r`n`r`n  <game name>.png or .jpg    600 x 900, named exactly as Switch Game shows the game:`r`n                             'Doom II.png', 'Heretic (2025 enhanced).jpg'`r`n  graphics\vrsplash.png      1920 x 1080, shown once at startup`r`n`r`nRun SETUP.bat after adding files; it packs this folder into boxart.pk3.`r`n", [Text.Encoding]::ASCII)
        }
        $oursDir = Join-Path $PSScriptRoot "boxart"
        # A cover: the player's own, else ours, else none ($null).
        function CoverFor([string]$name) {
            foreach ($d in $artDir, $oursDir) { foreach ($ext in ".png", ".jpg") { $c = Join-Path $d ($name + $ext); if (Test-Path -LiteralPath $c) { return $c } } }
            return $null
        }
        $made = 0; $titles = @{}
        foreach ($g in $writtenEntries) {
            $mine = CoverFor $g.name
            $auto = Join-Path $autoDir ($g.name + ".png")
            if ($mine -or (Test-Path -LiteralPath $auto)) { continue }
            $files = @(PathOf $g.iwad) + @($g.files | Where-Object { $_ -and $_ -notlike "mods\music_*" } | ForEach-Object { PathOf $_ })
            $t = [DoomVR.Art]::FindTitle([string[]]$files)
            [DoomVR.Art]::MakeCover($t, $g.name, $auto)
            if ($t) { $titles[$g.name] = $t }
            $made++
        }
        if ($made -gt 0) { Say ("Made {0} covers from your games' title screens (boxart\auto)" -f $made) }

        $splashMine = @("png", "jpg") | Where-Object { Test-Path -LiteralPath (Join-Path $artDir "graphics\vrsplash.$_") } | Select-Object -First 1
        $splashAuto = Join-Path $autoDir "vrsplash.png"
        # Game Or Die's own splash for Doom VR, shipped beside this script: the
        # player's own still wins; the one made from title screens is the last resort.
        $splashOurs = Join-Path $PSScriptRoot "vrsplash.png"
        if (-not (Test-Path -LiteralPath $splashOurs)) { $splashOurs = $null }
        if (-not $splashMine -and -not $splashOurs -and -not (Test-Path -LiteralPath $splashAuto)) {
            $pick = @()
            foreach ($n in "Doom (2024 re-release)", "Doom", "Doom II", "Heretic", "Hexen", "Doom 64", "Strife", "Final Doom - TNT", "Final Doom - Plutonia", "Heretic (2025 enhanced)", "Hexen (2025 enhanced)", "PSX Doom", "Chex Quest 3", "Freedoom Phase 1") {
                if ($writtenEntries | Where-Object { $_.name -eq $n }) {
                    $g = $writtenEntries | Where-Object { $_.name -eq $n } | Select-Object -First 1
                    $t = if ($titles.ContainsKey($n)) { $titles[$n] } else { [DoomVR.Art]::FindTitle([string[]](@(PathOf $g.iwad) + @($g.files | Where-Object { $_ } | ForEach-Object { PathOf $_ }))) }
                    if ($t) { $pick += $t }
                }
                if ($pick.Count -ge 12) { break }
            }
            if ($pick.Count -gt 0) { [DoomVR.Art]::MakeSplash([System.Drawing.Bitmap[]]$pick, "DOOM VR", $splashAuto); Say "Made the startup splash from your games' title screens" }
        }

        # boxart.pk3: rebuilt every run from what is there now.
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $pk3 = Join-Path $Install "boxart.pk3"
        $tmpPk3 = $pk3 + ".new"
        if (Test-Path -LiteralPath $tmpPk3) { Remove-Item -LiteralPath $tmpPk3 -Force }
        $zip = [IO.Compression.ZipFile]::Open($tmpPk3, "Create")
        $packed = 0
        try {
            foreach ($g in $writtenEntries) {
                $src = CoverFor $g.name
                if (-not $src) { $c = Join-Path $autoDir ($g.name + ".png"); if (Test-Path -LiteralPath $c) { $src = $c } }
                if (-not $src) { continue }
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $src, "boxart/" + $g.name + [IO.Path]::GetExtension($src), "NoCompression")
                $packed++
            }
            # Launchers setup did not write (added by hand) take the player's
            # own cover when there is one named like them.
            $ours = @{}; foreach ($g in $writtenEntries) { $ours[$g.name] = 1 }
            foreach ($b in Get-ChildItem -LiteralPath $ldir -Filter "*.bat" -File -ErrorAction SilentlyContinue) {
                if ($ours.ContainsKey($b.BaseName)) { continue }
                $c = CoverFor $b.BaseName
                if ($c) {
                    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $c, "boxart/" + $b.BaseName + [IO.Path]::GetExtension($c), "NoCompression")
                    $packed++
                }
            }
            $splash = if ($splashMine) { Join-Path $artDir "graphics\vrsplash.$splashMine" } elseif ($splashOurs) { $splashOurs } elseif (Test-Path -LiteralPath $splashAuto) { $splashAuto } else { $null }
            if ($splash) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $splash, "graphics/vrsplash" + [IO.Path]::GetExtension($splash), "NoCompression") }
            # Switch Game's backdrop: the player's own, else Game Or Die's shipped beside
            # this script; none, and Switch Game uses the splash.
            $backMine = @("png", "jpg") | Where-Object { Test-Path -LiteralPath (Join-Path $artDir "graphics\vrbackdrop.$_") } | Select-Object -First 1
            $back = if ($backMine) { Join-Path $artDir "graphics\vrbackdrop.$backMine" } elseif (Test-Path -LiteralPath (Join-Path $PSScriptRoot "vrbackdrop.png")) { Join-Path $PSScriptRoot "vrbackdrop.png" } else { $null }
            if ($back) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $back, "graphics/vrbackdrop" + [IO.Path]::GetExtension($back), "NoCompression") }
        } finally { $zip.Dispose() }
        Move-Item -LiteralPath $tmpPk3 -Destination $pk3 -Force
        Say ("Packed {0} covers{1} into boxart.pk3" -f $packed, $(if ($splash) { " and the splash" } else { "" }))
    } catch {
        Say ("Art: skipped - " + $_.Exception.Message)
    }
}

Say ""
Say ("{0} games ready: {1}" -f $written.Count, ($written -join ", "))
Say ("Modern VR style (the default; Switch Game > Style): add-ons for {0} of them - voxels, gore, sounds, music and textures that fit each" -f $nModern)
if ($missing.Count -gt 0) {
    Say ("Not found: " + ($missing -join ", "))
    Say "  Have them somewhere else - an older Doom VR folder, or another Doom port's? Drop that folder onto Setup.bat."
}
if ($start) { Say "Ready: $start starts first; Switch Game has the rest." } else { Say "No games found yet - nothing to start." }
Say "Setup finished."
