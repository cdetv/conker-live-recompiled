Conker: Live & Recompiled
=========================

A native Windows PC version of the original Xbox game Conker: Live &
Reloaded. The game's own code was translated ("recompiled") to run directly
on Windows, with sharper pictures, up to 120 fps, keyboard and mouse,
an unlocked camera, photo mode and more. It also runs on Linux and the
Steam Deck through Steam's Proton (see LINUX AND STEAM DECK below).

No game files are included: you need your own copy of the game.

Project page, updates and bug reports:
https://github.com/cdetv/conker-live-recompiled


WHAT YOU NEED
-------------

- Windows 10 or 11, 64-bit, and a graphics card with DirectX 11. Or
  Linux or a Steam Deck with Steam and its Proton 11.
- Your own copy of Conker: Live & Reloaded for the original Xbox, PAL
  (Europe) or USA, as a disc image (.iso, Redump or XISO) or as the
  disc's files. It may be packed in a .zip, .7z or .rar. Or the Xbox
  Store version, installed from the Xbox app.
- About 6 GB of free space (more while unpacking an archive).
- A controller is optional: Xbox controllers, PlayStation 4 and 5
  controllers (DualShock 4, DualSense), Switch controllers (Pro Controller,
  a pair of Joy-Cons; no extra programs needed), or keyboard and mouse.


GETTING STARTED
---------------

1. Unzip this folder anywhere you like, for example D:\Games\Conker.
   Avoid C:\Program Files: the launcher has to write into its own folder
   (game files, updates).

2. Start conker-launcher.exe.
   Windows may show "Windows protected your PC" the first time, because
   the programs aren't signed. Click "More info", then "Run anyway".

3. Import the game: on the Files tab click "Import Files" and pick your
   disc image, archive, or the default.xbe in the game's folder. The
   launcher checks it is a supported copy (PAL or USA) and copies the
   game into the "game" folder here (about 4.5 GB). Your original file is
   only read, never changed.
   Got the Xbox Store version? Install it from the Xbox app; the launcher
   finds it by itself: click "Import Xbox Store version".

4. Click Play.

Optional: Steam. "Add to Steam" on the Files tab adds the launcher to
your Steam library as a non-Steam game, with artwork from SteamGridDB
(Steam closes for a moment if it is open, then starts again).

Optional: the uncensored audio and text. On the Files tab, "Download
patch" fetches the fan-made decensor patch (by nastys, Vaani and
Theconker64) from its GitHub page; then turn on "Uncensored" in the
Settings tab or in the game's F2 menu.


LINUX AND STEAM DECK
--------------------

The same files run on Linux and the Steam Deck through Steam's Proton 11
(tested on a Steam Deck LCD; other Linux PCs with Steam should work the
same way). On a Steam Deck, set it up in Desktop Mode:

1. Unzip this folder into a folder of its own, for example ~/Games/Conker,
   and copy your disc image onto the computer.

2. Add it to Steam: in that folder, double-click add-to-steam.sh and
   choose Execute (or press F4 in the file manager for a terminal and type
   bash add-to-steam.sh). It closes Steam, adds conker-launcher.exe as
   "Conker: Live & Recompiled" with Proton 11.0 and its artwork from
   SteamGridDB, and starts Steam again. Running it again updates the same
   entry (moved the folder? run it from the new place).
   Or by hand: in Steam, Games > Add a Non-Steam Game to My Library >
   Browse, set the file type to All Files, pick conker-launcher.exe and
   click Add Selected Programs; then right-click it > Properties >
   Compatibility, tick "Force the use of a specific Steam Play
   compatibility tool" and choose Proton 11.0.

3. Start it from Steam and import the game as above. In the file browser
   your home folder is Z: > home > your user name (deck on a Steam Deck),
   and a Steam Deck's microSD card is D:.

4. Click Play. From then on it starts from your Steam library, in Gaming
   Mode too. Back + Start > Close Game takes you back to Steam.

The project page's README has more for the Steam Deck: battery life,
Precompile Shaders, the controls, gyro aiming.


IN THE GAME
-----------

  F2            Settings menu (graphics and quality presets, sound,
                frame rate, ...)
                On a controller: Back + LB
  F3            Controls menu (keys, mouse, controllers per player)
  F4            Photo mode (on a controller: Back + RB; Esc leaves)
  Back + Start  On a controller: a menu with settings, controls, photo
                mode, Report Bug, controller options and Close Game
  F8            The cheat menu, once you've unlocked it
  F10           Report a bug: makes a .zip to attach to a GitHub issue (on a
                controller: the Report Bug... button in the F2 menu)
  F11           Full screen on/off (also Alt+Enter)
  F1            Frame rate counter (press again for frame times, a third
                time for what limits your frame rate)
  P             Pause or resume the game (gameplay and cutscenes)
  Mouse wheel   Zoom the camera in or out (Unlocked Camera; on a
                controller: hold D-pad up and push the right stick)
  V             Put the camera back behind Conker (on a controller:
                right stick click)
  Esc           On the main menu: close the game (asks first; on a
                controller: B)
  Hold LT       Skip a cutscene (keyboard: hold Space, E or the left
                mouse button)

Every setting is also in the launcher's Settings and Controls tabs, so you
can set things up before playing.


EXTRAS
------

F2 > Extras holds options that change the game itself (single player).
All are off by default, as the original, and none is kept in your saves:

- Melee: Quick Swing (hold attack to keep swinging), Forgiving Combo,
  Fast Weapon Draw, Camera Switch (the camera button switches to over the
  shoulder) and Swing While Jumping.
- Unload Reloaded (its checkbox turns all of these on or off at once):
  - Weapon: the bat from the start, or the frying pan all game long
    (Update Cutscene makes the training's weapon scenes match).
  - Milk Fix: King Conker holds a glass of milk in the opening scene, as
    on the N64.
  - No Key Goblings: the training cave's key hops out without the goblins.
  - Free Swing: Reload (X) or the Attack key takes the bat or frying pan
    out, swings at once and puts it away again.

The frying pan and the glass of milk are made from your own game files.


DEBUG MODE
----------

Tick "Debug Mode" on the launcher's Debug tab (or start conker.exe
--debug) for a Debug menu (F8) and a console (~): level warp, a cutscene
viewer, noclip, Edit Save, a jukebox and more, and every cheat without its
code. DEBUG_MODE.txt in this folder lists everything it does. It's a
testing tool: back up your saves (the userdata folder) first.


TEXTURE PACKS
-------------

A texture pack swaps the game's textures for new art. Unzip a pack into
textures\packs next to conker.exe and tick it in F2 > Extras > Texture
Packs (or in the list on the launcher's Files tab). To make your own,
Export Textures on the launcher's Files tab saves every texture as a PNG
to edit. TEXTURE_PACKS.txt in this folder explains how.


WHERE YOUR FILES ARE
--------------------

Saves, settings.ini, photos, logs and the shader cache live in the userdata
folder next to conker.exe (use the Saves, Photos and Logs buttons on the
launcher's Files tab). To back up your saves, copy that folder. Updates
never touch it, but deleting the game's folder deletes your saves with it.

Each save has its own folder in userdata\saves: Game1, Game2 and Game3 for
the story, and one named after each multiplayer profile. Saves from older
versions (folders like 10C2167B1950) are renamed automatically.

The launcher's Files tab can keep them in AppData instead
(%LOCALAPPDATA%\conker-lr-recomp\), which is also where versions before
0.1.3 kept them: the launcher moves those into the game folder by itself.
If both places hold the same save slot, it asks what to do; backups go to
the backups folder inside userdata. If the game's folder can't be written
to (for example under Program Files), AppData is used.


UPDATES
-------

The launcher checks for a new version each time it starts. When there is
one, click "Update": it replaces the program files and keeps your game
files, the decensor patch and anything you added.


KNOWN ISSUES
------------

- Xbox Live and System Link aren't available. Multiplayer against bots
  (Dumbots) works.
- The first time an area is seen, a short pause can happen while the
  graphics card prepares it; it is remembered for next time. "Precompile
  Shaders" on the launcher's Files tab prepares the story's shaders
  ahead (a few minutes, once).


PROBLEMS?
---------

- "Windows N" or "KN" editions: add Microsoft's Media Feature Pack (in
  Windows Settings, search for "Optional features", add "Media Feature
  Pack"), or the game's movies can't play.
- "Game files not imported yet": start the launcher and import the game
  (step 3 above).
- Anything else: please report it on the project page (Issues). F10 in the
  game, or Report Bug on the launcher's Files tab, makes a .zip to attach:
  the game's logs, a picture of the moment, your settings and, if you leave
  them ticked, your saves. After a crash the game offers this by itself, and
  F10 still works if the game has frozen.


LICENCE AND CREDITS
-------------------

The project's own code is MIT licensed (LICENSE.txt). It is built with the
xboxrecomp toolkit by Ned Heller (sp00nznet) and contributors, the xemu
sound emulation (LGPL, in xemu_apu.dll), the Full DSP sound effects and
the ambient occlusion after yacineolivier-maker's ConkerLR-Recomp (MIT),
the package reader, the
Frying Pan, the Milk Fix and Free Swing after Txukie's xboxrecomp-conker
(MIT), Dear ImGui, SDL3, AMD FidelityFX CAS and FSR 1, NVIDIA FXAA, SMAA,
.NET and Avalonia; the full list, with every licence, is
THIRD_PARTY_NOTICES.txt.

The Steam artwork that Add to Steam and add-to-steam.sh download is not
part of this download: it is shared on SteamGridDB by PopASquatt (cover,
wide cover and logo), Jiquita (background) and EVE Online (icon).

Conker: Live & Reloaded, Conker and Xbox are trademarks of Microsoft
Corporation / Rare Ltd. This project is not affiliated with or endorsed by
Microsoft or Rare.
