# Debug mode

Debug mode (from 0.3.0) opens up the game for testing, exploring and messing around: a
**Debug menu**, a **console** for typed commands, every **cheat** without its code, and a few
extra choices in the settings menu.

> **It's a testing tool.** Level warps, Edit Save and some of the tools put the game in
> states the story never does. Before you experiment, copy your `userdata\saves` folder
> somewhere safe. Found a bug? Say in the report that debug mode was on (the F10 report
> notes it too).

## Turning it on

- **Launcher:** the **Debug** tab, tick **Debug Mode**. It stays ticked until you untick it;
  **Play** then starts the game in debug mode.
- **Without the launcher:** start `conker.exe --debug` (for example from a shortcut whose
  target ends in `--debug`).

## What's different in debug mode

| | Key | Controller |
|---|---|---|
| Debug menu | F8 | Back + Start, then **Debug** |
| Console | `~` (the key left of 1, on any keyboard layout) | |
| Noclip on / off | Z | RB + A |
| Freecam: control the camera or Conker | G | Back + X |

- Every cheat is available without its code (the Debug menu's **Cheats** row).
- The settings menu (F2) gets a few extra choices, marked "(debug)": see
  [Extra settings](#extra-settings).
- The multiplayer menu shows Xbox Live and System Link again, as on the Xbox. They still
  don't work.

## The Debug menu (F8)

A small window over the game, which keeps running under it. It remembers where you put it.

- **Cheats**: the cheat menu with every cheat. **Unlock cheats** unlocks the cheat menu for
  the normal game too, as finding its code would; once unlocked the button becomes **Reset
  cheats** (locked and hidden again, every cheat off).
- **Level warp**: pick a level, then its first checkpoint, any checkpoint, or **Soak**
  (the level as the game's soak test loads it). Nothing is saved until you go back to the
  main menu; a small note on screen says so (it can be turned off under Options).
- **Cutscene viewer**: pick a level and one of its scenes and watch it. Afterwards you are
  back where you were; the story doesn't move on and nothing is saved. Hold the skip button
  to skip any scene.
- **Tools**: noclip (fly through walls and floors; nothing can kill Conker), freecam (a free
  camera while the world keeps running), wireframe (characters and objects, or the whole
  scene, drawn as lines) and timescale (the game's speed, 0 to 300 %).
- **Edit Save**: moves Game 1, 2 or 3 to the start of a chapter's part, exactly as picking it
  from the Chapters list would. A copy of the old save goes to `edit_save_backups\` first.
  Not for the save being played: go back to the main menu first.
- **Jukebox**: the game's sounds by name, and its music tunes. A tune from another level
  loads its music first (about 2 seconds).
- **Rare's debug pages**: the developers' own pages printed over the picture (memory,
  performance, positions, camera, character, effects, ...). Previous / Next, or LB / RB on the
  controller. A page stays up when the menu closes.
- **Soak test**: the game's own test that visits every map in turn (about 8 seconds each),
  then the chapters, over and over. From the main menu; close the game to stop it.
- **Options**: the note after a level warp, and the old P pause (holds the last picture
  instead of freezing the game).

Also in debug mode: `warp FrontendTable` (or the cutscene viewer's FrontendTable 1) plays the
game's unused N64-style intro all the way through, then goes on to the main menu. Some parts are
missing on the Xbox disc: it has no speech, and some textures are blank.

## The console (`~`)

- **Open / close:** `~`. Esc also closes it while you type.
- **Typing:** Enter runs the line, Tab completes a command's name (Tab after a name and a
  space shows how to use it), Up / Down bring back earlier lines, Page Up / Page Down scroll.
- **Copy and paste:** drag over lines with the mouse to select them, then Ctrl+C or right
  click, Copy. Ctrl+V pastes.
- **The game keeps running** and keeps the controller; the keyboard belongs to the console.
  A left click outside the console gives the keyboard and mouse back to the game with the
  console still showing; `~` takes them back.
- **Not case sensitive** (`WARP`, `warp` and `Warp` all work). Numbers can be decimal or hex
  (`0x7A1`).
- `help` lists every command; `help <command>` shows one.

How the usage is written: `<word>` = you must give it, `[word]` = optional, `a|b` = one of
these.

### Getting around

| Command | What it does |
|---|---|
| `warp list` | Lists the levels you can warp to, by number and name. |
| `warp <level>` | Goes to the level's first checkpoint. The level is its number or the start of its name (`warp Windy`). |
| `warp soak <level>` | Loads the level as the soak test does: progress wiped, nothing started yet. Its exits don't work. |
| `warp cp list [level]` | Lists the checkpoints, all or one level's. |
| `warp cp <number>` | Starts a new game at that checkpoint, with only the progress before it. |
| `warp scene <level> <scene>` | Starts at the level's first checkpoint and plays that cutscene; the story carries on from there. |
| `cutscene list [level]` | Lists the levels with cutscenes and their scene numbers. |
| `cutscene <level> <scene>` | Plays that scene, then brings you back where you were. Nothing is saved. |
| `noclip [on\|off]` | Fly through walls and floors (left stick, RT / LT up and down; keyboard W, Space, Ctrl). Nothing can kill Conker; turning it off in the air lands him safely. Stays on across warps. |
| `freecam [on\|off]` | Photo mode's fly camera while the world keeps running. G (Back + X) swaps between the camera and Conker. |
| `pos` | Says where Conker is (x y z) and in which level. |
| `tp <x> <y> <z>` | Puts Conker at those coordinates. A drop from there does no damage. |
| `tp <number>` | Puts Conker beside a character (numbers from `chars`). |
| `tp cam` | Puts Conker where the freecam is. |
| `spot save <name>` | Saves where Conker stands under a name (one word), per level. |
| `spot go <name>` / `spot delete <name>` / `spot list` | Goes there, removes it, lists this level's spots. |
| `chars [count]` | Lists the level's characters, nearest first: number, type, distance and energy. |

Warp and the cutscene viewer don't work in a multiplayer match, with the pause menu open or
while a level loads. No value after `on|off` switches a tool on or off.

### Conker and saves

| Command | What it does |
|---|---|
| `set energy <1-6>` | Conker's energy. |
| `set lives <0-100>` | His lives. |
| `set cash <0-1000000>` | His money. |
| `set oxygen <0-72>` | His air, in seconds. |
| `kill` | Conker dies on the spot: a life is lost and the checkpoint reloads. |
| `editSave list` | Lists the chapter words for Edit Save: `hung`, `windy`, `bat`, `barn`, `slop`, `uga`, `spooky`, `war`, `heist`. |
| `editSave <1\|2\|3> <word><number>` | Moves Game 1, 2 or 3 to that part (`editSave 1 slop2` = Sloprano part 2). A backup is made first. |

`set` and `kill` work in story levels only.

### Cheats

Every cheat as a command: `<command> [on|off]` (no value switches it). They work in the story
only, except Multiplayer unlocks.

| Command | Cheat | What it does |
|---|---|---|
| `godmode` | God mode | Energy never drops; falls and drowning don't hurt. Pits and scripted kills still reload, no life lost. |
| `reloading` | No reload | Story guns never need reloading. |
| `rosebud` | $1,000,000 | The game being played holds 1,000,000; off = the old amount back. |
| `inftail` | Infinite tail spin | Hold A to hover with the tail spinning; let go to glide down; crouch to sink faster. |
| `allchapters` | Unlock all chapters | The Chapters list shows all 62 parts; off puts the list back. |
| `infoxygen` | Infinite oxygen | Conker never runs out of air. |
| `inflives` | Infinite lives | Losing a life doesn't lower the count. |
| `mpunlocks` | Multiplayer unlocks | Potty Mouth, Flame Fire and Purple Streak available; your real profile is not changed. |
| `glass` | Glass Squirrel | One hit and Conker dies. God mode wins if both are on. |
| `bullettime` | Bullet time | Then B (or both sticks clicked) switches slow motion on and off. |
| `moonjump` | Moon Jump | A third of the gravity in the air: jumps go about 2.5 times as high and float down; falls don't hurt. |
| `superspeed` | Super Speed | Conker covers twice the distance walking, running and in the air. |
| `bighead` | Big Head | Conker's head drawn twice as big. |
| `ironboots` | Iron Boots | Conker doesn't swim: he sinks and walks on the bottom. His air still runs out. |
| `holysquirrel` | Holy Squirrel | Conker walks on water. Wins over Iron Boots when both are on. |
| `diveanywhere` | Dive Anywhere | Conker can dive in any water, also before he has learnt to dive. |

### The game and the picture

| Command | What it does |
|---|---|
| `timescale [0-300]` | The game's speed in percent (100 = normal), in the menus too. No value swaps with the last speed. `0` freezes the game (P resumes at 100). Sound stays at normal speed. |
| `wireframe [models\|all\|off]` | `models`: characters, what they hold and placed objects as lines; `all`: the whole scene as lines on black. |
| `soak` | Starts the game's soak test (from the main menu; close the game to stop it). |
| `debugpage [<number>\|next\|prev\|off]` | Rare's debug pages over the picture; no value lists them. |
| `metrics [0\|1\|2]` | The F1 counter: off, fps, or fps with frame times and memory. |

### Sounds

| Command | What it does |
|---|---|
| `cue <number>` | Plays one of the game's sounds (1 to `0xDD9`). The Debug menu's Jukebox lists them by name. |
| `tune list [part of a name]` | Lists the game's music tunes with their numbers. |
| `tune <number\|name>` | Plays a tune in place of the music playing (a tune from another level loads its music first). A few only play in their own level. |
| `tune stop` | Brings the replaced music back. |

### Key binds and autoexec

| Command | What it does |
|---|---|
| `bind <key> <command>` | That key runs the command (`bind f5 warp Windy`, `bind f6 noclip`). Key names are the F3 menu's. Saved in `settings.ini`. |
| `bind list` | Lists the bound keys. |
| `bind reset` | Removes every bind. |
| `unbind <key>` / `unbind all` | Removes one bind, or all. |

Binds work in debug mode whenever no menu is taking the keys.

**`autoexec.txt`:** put a file with this name in your save folder (`userdata`), one command per
line. It runs once, 2 seconds after the main menu first appears. Lines starting with `#` or
`;` are comments.

```
# my test setup
godmode on
bind f5 warp cp 193
metrics 1
```

### The console itself

| Command | What it does |
|---|---|
| `help [command]` | Lists every command, or shows one. |
| `clear` | Empties the console. |
| `con_out [simple\|detailed\|full]` | What the console shows: only command replies, also the game's tagged log lines, or the whole log. |

## The launcher's Debug tab

- **Debug Mode**: on, **Play** starts the game in debug mode (remembered).
- **Edit Save**: pick a game (1, 2 or 3), a chapter and a part, then **Edit Save**. The save
  then starts at the beginning of that part, as the Chapters list would start it. It asks
  first, and doesn't work while the game is running. The same as the Debug menu's Edit Save.

## Extra settings

In debug mode the settings menu (F2) also offers:

- **Render scale** up to 10x (normally up to 4x).
- **Frame rate** 240 fps and uncapped (normally up to 120).
- **Keep speed in slowdowns** (Display, at 30 fps; on by default): untick it to bring back
  the original game's speed-up when it slows down.
- **Physics at 30 Hz** (Fixes; on by default): above 30 fps, tails, ragdolls and cloth move
  at the original 30 Hz.
- **Tail-shaped ring** (Fixes, under Cutscenes): the hold-to-skip ring drawn as Conker's
  curled-up tail.
- **Sound**: a **Solo** box per sound channel, and two more speaker mixes for testing.
- **F1** at level 2 also shows memory and video card use.
