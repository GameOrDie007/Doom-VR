Doom VR for Quest 3 and Steam Frame (standalone)
=================================================

The same Doom VR as on the PC, running on the headset with no PC while you play:
Doom, Doom II, Final Doom, DOOM + DOOM II (2024) with Legacy of Rust, Heretic,
Hexen, Strife, Chex Quest, the free games and the mods, with Switch Game, our 3D
guns and hands, physical play and the rest. The PC version stays the main one:
every game and mod comes to the headset from it.

Before you start
----------------
1. Install Doom VR's PC version on this PC and run its Setup once. It finds the
   Doom-family games you own (Steam, GOG, or a folder you drop on it), fetches
   the free games and mods, and keeps them in its VR\game folder. That folder is
   what goes to the headset. (Without a PC install, this Setup puts the same
   folder together itself; it takes longer.)
2. Quest 3: turn on Developer Mode (Meta Horizon app on your phone: Menu,
   Devices, your headset, Headset Settings, Developer Mode). Plug the headset
   into this PC with a USB-C cable that carries data.
3. Steam Frame: Developer Mode on (Settings, System), Lepton installed (Store);
   this PC pairs with it once (on the Frame: Settings, Developer, Pair new host,
   then Accept when Setup asks).

Install
-------
Quest 3:      Setup.bat             (the headset plugged in; Allow on the headset the first time)
Steam Frame:  Setup Steam Frame.bat (over your home network)

Setup installs the app and copies the games. Run it again any time: it copies
only what changed, and never touches your settings or saved games.

Play
----
Quest 3:      Library, Unknown Sources, Doom VR.
Steam Frame:  Library, Doom VR (its own entry, with its art).

No menu button on your link (some Virtual Desktop setups)? Stand still and
hold the left stick in for a second: it opens the menu too.

The game picker opens first, as on the PC; Switch Game (main menu, or Options)
has every game. Your settings from the PC come over the first time, so your
hand fits and options are the same.

Uninstall, logs
---------------
Uninstall.bat / Uninstall Steam Frame.bat   remove the app; your settings and
    saves are copied to Documents\Doom VR first.
Collect logs.bat / Collect Steam Frame logs.bat   one zip on your desktop, ready
    to send us (Discord or GitHub). Nothing is sent anywhere by itself.

For tools and hubs
------------------
Every .bat passes its arguments to files\setup-quest.ps1:
  -Quiet  -Json  -Detect  -GamePath "<Doom VR game folder>"  -Serial <Quest id>
  -Frame <name or address>  -RefreshHz <n>  -NoDownload  -Mirror <url>
Exit codes: 0 done, 1 no games, 2 error, 3 no headset.

Doom VR is a Game Or Die port. UZDoom is GPL-3.0-or-later; see the PC release
for the licenses of everything in it. No game data ships with this download.
