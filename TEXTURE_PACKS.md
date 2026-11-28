# Texture packs

A texture pack swaps the game's textures, the pictures painted on walls, characters,
items and menus, for new art: sharper remakes, new colours, jokes. A pack is just a folder
of PNG files, so all you need to make one is an image editor. Texture packs came in 0.7.0.

## Using a pack

1. **Put it in place.** Unzip the pack into the `textures\packs` folder next to
   `conker.exe` (the launcher's **Files** tab, **Texture packs**, **Packs Folder** opens
   it). You should end up with `textures\packs\<the pack's name>\`, and the pack's own
   folders (usually `zpackage`, maybe `texture`) inside that.
2. **Turn it on.** In the game: **F2**, the **Extras** tab, **Texture Packs**, and tick the
   pack. Or before you start: the same list on the launcher's **Files** tab. A new pack
   starts off.
3. **Several packs?** The top one wins where two packs change the same texture. To move a
   pack, drag it with the mouse, or hold **A** (or **Space**) and press up or down.

In the game, a change applies at once, with a short pause while the textures are made
again. The launcher's list is locked while the game runs: change it in the game's F2 menu
then. Both save to the same setting, so the game starts with the packs you left on.

## Making a pack

### 1. Export the textures

On the launcher's **Files** tab, under **Texture packs**, click **Export Textures**. It saves
every texture a pack can change as a PNG, taken from your own game files: about 9,600 files
and 600 MB, in a minute or so. They go into `textures\dump` (**Dump Folder** opens it).
**Cancel** stops it; **Export Again** goes on where it stopped.

The game never reads the dump folder: changing a file there does nothing. It's your
reference to copy from.

### 2. Find the textures you want

The dump keeps the game's own folders and file names. Rare's names, but they mostly say what
they are:

| Folder | What's in it |
|---|---|
| `zpackage\Level\<level>\` | Each level's own textures (`Level\BarnInside` is the inside of the barn, `Level\Frontend` the main menu) |
| `zpackage\General\Singleplayer\` | What every story level uses, Conker himself included |
| `zpackage\General\Multiplayer\`, `zpackage\Characters\`, `zpackage\TimeZone\` | Multiplayer: its characters, weapons and vehicles (`TimeZone\WWII` and `TimeZone\Future`) |
| `zpackage\Effects\` | Effects of a couple of levels |
| `texture\` | Multiplayer menu pictures: avatars, level pictures, tips |

Tips:

- Explorer's **Large icons** view shows the pictures, so you can browse a folder by eye.
- A picture the game uses in several places is exported once, in the first folder that has
  it. Change it and it changes everywhere it is used.
- Names ending in `@1`, `@2`, ... are the frames of one texture: Conker's eyes, for example,
  are `New_Conker_Eye.1`, then `New_Conker_Eye.1@1`, `@2` and so on. Change each frame you
  want changed.
- `textures.tsv` in the dump folder lists every use of every texture: its size, its format
  and the level or package it is in. It opens in any spreadsheet program; search its `png`
  column for a file name to see everywhere that picture is used.

### 3. Make your pack's folder

Make a folder for your pack in `textures\packs`, and copy into it **only the PNGs you
change**, keeping the folders below `dump` exactly as they are and the file names unchanged:

```
textures\dump\zpackage\Level\BarnInside\BarnInside_texturefile34.png
                              becomes
textures\packs\My Pack\zpackage\Level\BarnInside\BarnInside_texturefile34.png
```

Upper or lower case doesn't matter. Don't copy the whole dump in: a pack works with any
number of files, and textures you didn't change only cost memory and loading time.

### 4. Edit

Any image editor that saves PNG will do.

- **Any size works.** Make it 2 or 4 times bigger for a sharper look; the game makes the
  smaller versions it needs itself. Keep the same shape (the same width to height), or the
  picture is stretched over the same spot. Bigger costs memory: 4 times the width and
  height is 16 times the pixels.
- **Keep transparency** where the original has it (fur edges, leaves, decals): the exported
  PNG shows where it is.
- **Distortion maps:** 67 files aren't pictures but tell the game how to ripple or bend
  what's behind them (water, foam, wet footprints, glass). The game may ignore a new version
  of them. `textures.tsv`'s `replace` column says "maybe" for these and "yes" for every
  other file in the dump.

### 5. Try it

Turn your pack on in the game (**F2**, **Extras**, **Texture Packs**) and go where the
texture shows. A new pack folder shows up in the list within a few seconds, off.

**Changed a PNG while the game runs?** Untick your pack and tick it again: the game then
reads its files again. No restart needed.

## Sharing a pack

Zip your pack's folder (the folder itself, so the zip holds `My Pack\zpackage\...`) and
tell players to unzip it into `textures\packs`. They don't need to export anything: the
game finds each texture by its name in their own game files.

## What a pack can't change

- **Reflections** (cube maps) and **3D textures** (volume textures, such as the light on the
  barn floor): the game draws these in a way a flat PNG can't stand in for, so they aren't
  exported.
- The **movies** and pictures the game makes while it runs (screen effects): they aren't
  among the textures in the game's files, so they aren't in the dump either.
- The **text font** isn't in the dump either, but it can be changed by hash (below): the
  sharper set's font sheets, in the `assets\sharp` folder next to your saves, are named by
  their hash. Keep the same layout; any multiple of the size works.

Everything in the dump can be changed (the distortion maps above perhaps not).

The game's own **sharper text and buttons** (F2 → Display → Sharper Text and Buttons, on by
default) sit under every pack: a pack that changes a button picture or the font wins over
them.

## If a texture doesn't change

- Is the pack **ticked**? Does a pack **higher up** change the same texture?
- Is the path right? `textures\packs\My Pack\zpackage\...`, not
  `textures\packs\My Pack\My Pack\zpackage\...` (a zip unpacked one folder too deep), and
  not in `textures\dump`.
- Is the **file name** exactly the exported one, still ending in `.png`?
- Did the launcher **prepare the game assets** (the Files tab's **Game assets**)? It does by
  itself after an import or an update, and packs need the list of texture names it makes.
- The **log** says what each pack holds: the launcher's **Logs** button, the newest
  `conker_<date>_<time>.log`. Lines starting with `[TEXREP]` give each pack's number of
  textures, and the first PNG whose name matches no texture.

## Names by hash (for modders)

Every texture also has a hash: 16 hex digits, the `hash` column of `textures.tsv`. A PNG
whose name **starts with** the hash (`4A68A8ABD73395C7_wall.png`) replaces that texture from
any folder in a pack, and also right in `textures\` itself (the older way, from before
packs: always on, under every pack). Hash names work even without the prepared game assets.
