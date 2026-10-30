Conker: Live & Recompiled
=========================

A native Windows PC version of the original Xbox game Conker: Live &
Reloaded. The game's own code was translated ("recompiled") to run directly
on Windows, with sharper pictures, 60 fps, keyboard and mouse, PlayStation
controllers, photo mode and more.

No game files are included: you need your own copy of the game.

Project page, updates and bug reports:
https://github.com/cdetv/conker-live-recompiled


WHAT YOU NEED
-------------

- Windows 10 or 11, 64-bit, and a graphics card with DirectX 11.
- Your own copy of Conker: Live & Reloaded for the original Xbox, PAL
  (Europe) or USA, as a disc image (.iso, Redump or XISO) or as the
  disc's files. It may be packed in a .zip, .7z or .rar.
- About 5 GB of free space (more while unpacking an archive).
- A controller is optional: Xbox controllers, PlayStation 4 and 5
  controllers (DualShock 4, DualSense; no extra programs needed), or
  keyboard and mouse.


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

4. Click Play.

Optional: the uncensored audio and text. On the Files tab, "Download
patch" fetches the fan-made decensor patch (by nastys, Vaani and
Theconker64) from its GitHub page; then turn on "Uncensored" in the
Settings tab or in the game's F2 menu.


IN THE GAME
-----------

  F2            Settings menu (picture, sound, frame rate, ...)
                On a controller: Back + LB
  F3            Controls menu (keys, mouse, controllers per player)
  F4            Photo mode (on a controller: Back + RB; Esc leaves)
  F11           Full screen on/off
  F1            Frame rate counter
  P             Pause or resume the game

Every setting is also in the launcher's Settings and Controls tabs, so you
can set things up before playing.


WHERE YOUR FILES ARE
--------------------

Saves, settings.ini, photos, logs and the shader cache live in

  %LOCALAPPDATA%\conker-lr-recomp\

(paste that into the Explorer address bar, or use the Saves, Photos and
Logs buttons on the launcher's Files tab). To back up your saves, copy that
folder. Deleting the game's folder never touches it.


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
  graphics card prepares it; it is remembered for next time.
- Sound can crackle for a moment when the game slows down.
- The original loading screen doesn't show while a level loads.


PROBLEMS?
---------

- "Windows N" or "KN" editions: add Microsoft's Media Feature Pack (in
  Windows Settings, search for "Optional features", add "Media Feature
  Pack"), or the game's movies can't play.
- "Game files not imported yet": start the launcher and import the game
  (step 3 above).
- Anything else: please report it on the project page (Issues), and
  attach the newest log from the Logs folder (launcher, Files tab, Logs).


LICENCE AND CREDITS
-------------------

The project's own code is MIT licensed (LICENSE.txt). It is built with the
xboxrecomp toolkit by Ned Heller (sp00nznet) and contributors, the xemu
sound emulation (LGPL, in xemu_apu.dll), Dear ImGui, SDL3, AMD FidelityFX
CAS and FSR 1, NVIDIA FXAA, SMAA, .NET and Avalonia; the full list, with
every licence, is THIRD_PARTY_NOTICES.txt.

Conker: Live & Reloaded, Conker and Xbox are trademarks of Microsoft
Corporation / Rare Ltd. This project is not affiliated with or endorsed by
Microsoft or Rare.
