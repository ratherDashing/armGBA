# armGBA-3DS

armGBA on the New Nintendo 3DS: Game Boy Advance games on armGBA's own core, with its recompiler
rebuilt for the 3DS's ARM11, and Game Boy and Game Boy Color games on Gambatte, as in the libretro
core. It's a standalone app (a `.cia` for the HOME Menu, or a `.3dsx` for the Homebrew Launcher)
with a small frontend of its own: the game on the top screen, the menu on the bottom one.

> **Status: 0.0.3-alpha.** 0.0.1 was played on one New 3DS, with two games measured closely (Mario
> Kart: Super Circuit and Advance Wars, both at full speed). What 0.0.2 added (Threaded Audio, the
> shorter recompiled code, the core's changes since) has run on a New 3DS inside PokeDaisy, with
> Pokemon Emerald; 0.0.3 fixes a freeze of 0.0.2's (a render worker that slept through its wake-up),
> found and checked in the Azahar emulator. Expect bugs; keep copies of saves you care about. Reports are welcome, with
> `sdmc:/3ds/armgba/log.txt` attached.

## What you need

- A **New 3DS, New 3DS XL or New 2DS XL** with **Luma3DS**. The recompiler needs to make memory
  executable, which Luma3DS allows homebrew to do. The original 3DS and 2DS aren't supported: the
  app starts on them, but their 268 MHz CPU, with no cores to spare for drawing, is far too slow.
- For sound: the DSP firmware dumped to `sdmc:/3ds/dspfirm.cdc` (run the *DSP1* homebrew once).
  Without it the games run silent.
- Your own games. armGBA-3DS comes with no games and downloads none.

## Install

- **HOME Menu:** install `armGBA-3DS.cia` with FBI. Each release's page has a QR code: FBI >
  Remote Install > Scan QR Code installs it straight from there.
- **Homebrew Launcher:** copy `armGBA-3DS.3dsx` to `sdmc:/3ds/`.

Put games anywhere on the SD card; the browser starts in `sdmc:/roms` and remembers the last folder.
It opens `.gba`, `.gb`, `.gbc`, `.bin` and `.agb` files, and the first game inside a `.zip` or
`.7z`, so a No-Intro set works as it is.

The app keeps its files in `sdmc:/3ds/armgba/`:

| Path | What |
| --- | --- |
| `system/` | Optional BIOS and boot ROMs: `gba_bios.bin`, and the Game Boy ones as the libretro core names them. Without them armGBA uses its own open-source ones |
| `saves/` | Game saves, `<game>.srm` and `.rtc`, the same files RetroArch makes: copy them either way |
| `states/` | Save states, `<game>.state` (slot 0) to `.state9` |
| `warm/` | For each game, a list of the code the recompiler has translated, so the next time it's translated ahead of need (see How the port works). Safe to delete |
| `config.ini` | The screen settings and the core options that differ from the defaults |
| `log.txt` | What happened last run (`log-prev.txt`: the run before): attach it to a bug report |

## Controls

| 3DS | Game |
| --- | --- |
| A, B, L, R, Start, Select | The same buttons |
| D-pad or Circle Pad | D-pad |
| Y in the game browser | Settings |
| X, Y (held) | Turbo A, turbo B |
| ZL, ZR (held) | Fast-forward |
| Touch the bottom screen | The menu |

While a game runs the bottom screen is dark, with its backlight off; touching it still opens the
menu. The menu has save and load state (ten slots), the screen (see below), smoothing,
**Settings**, restart, and close game. Settings (also **Y** in the game browser) holds armGBA's
options under headed sections: Display (the 3DS screens, Video, LCD Effects), Game (System, Clock,
Audio, Input) and Saves. Each option shows its values and its help text; L and R step ten values
at a time through the long lists, X puts an option back to its default (a `*` marks one that
isn't). Options that do nothing for the game loaded are hidden, as in RetroArch.

**Settings > 3DS Screens > Bottom Screen in a Game > Performance** shows the frame rate and where
each frame's time goes (emulation, waiting for the picture, translating, drawing on each core),
and writes the same to `log.txt` once a second.

## The picture

- **Pixel for pixel**, **Fit** (as large as fits, 1.5x for the GBA) and **Stretch**, with optional
  smoothing.
- **LCD3x**: libretro's `lcd3x` shader, as the 3DS can draw it. The 3DS's GPU has no programmable
  pixel shaders, so a GLSL/slang shader can't be loaded; instead the top screen runs in its
  800-column mode, the game is drawn three columns to a pixel (lcd3x's 3x across), and a mask
  holding lcd3x's own factors at every screen pixel multiplies it on the GPU: the red, green and
  blue stripes across ((4 + sin)/5) and the scanlines down ((16 + sin)/17). Across it's exact; down,
  the screen has 1.5 rows to a GBA pixel, so the scanlines are sampled there. It costs the CPU
  nothing.
- **armGBA's CPU LCD Effects** (Settings > Display > LCD Effects): ghosting, interlaced rows, the
  pixel grid and gaps, subpixels and the pixel shadow. LCD Scale is Off or Fit Screen here (the
  top screen's size). They run on the CPU and cost a lot of it: try LCD Ghosting alone first.

Defaults that differ from the libretro core, for the 3DS: **Skip BIOS Intro** is on, **Frame
Skip** is Auto (the core skips drawing frames when the sound is about to run short), and
**Threaded Video** is On (see below; One Frame Late is there too).

## Performance

Measured on a New 3DS, from the app's own log, each frame having 16.7 ms:

| | Frame rate | Emulation, a frame | Drawing, each of two cores |
| --- | --- | --- | --- |
| Mario Kart: Super Circuit, races | 59.7 fps | 9 to 12.6 ms (14.5 while a first lap is translated) | 3.7 to 5.7 ms |
| Advance Wars | 59.7 fps | 12.6 ms on average | about 2 ms |
| Advance Wars, the intro scene with an interrupt every line | 59.7 fps | 14.6 to 15.3 ms | about 2 ms |

With Threaded Video off (everything on one core) the same Mario Kart races took 14.5 to 16.1 ms
and the Advance Wars scene 16.5 ms, dropping to 58 fps. The first time a game reaches new code it
is translated, which can cost a few frames (a Mario Kart race's first start fell to 56 fps for a
second); the warm list is there so that happens once.

If a game runs slow, try in Settings: **Frame Skip** (Auto is on), **CPU Clock** below 100% (eases
slowdown in the game itself), and leave **CPU LCD Effects** and **GBA Color Correction** off.

## Building

With Docker, from the repository's root:

    docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src/platform/3ds devkitpro/devkitarm make -j4

That makes `platform/3ds/armGBA-3DS.3dsx`. `make JIT=0` builds the interpreter alone (about 12
times slower, but useful to check the recompiler against). `make cia` also makes
`armGBA-3DS.cia`, with `bannertool` and `makerom` (from Project_CTR) on the `PATH`; makerom's
release binaries need glibc 2.38 or newer, so run that step on the host or in a newer container.
The icon, the banner and its chime are drawn by `art/make.py` (Python with Pillow).

The GitHub workflow (`.github/workflows/3ds.yml`) builds both on every push. **A release** is a
tag named `3ds-v<version>` with a GitHub release published for it: the workflow attaches the
`.cia` and `.3dsx` and adds to the release's page a QR code of the `.cia`'s download link, for
FBI to scan. The version itself is `RELEASE` (and `MAJOR`, `MINOR`, `MICRO`) in the Makefile.

## Development builds

`make LOG_URL=http://<host>:<port>/log` builds one that, at start, POSTs the last run's log
(`log-prev.txt`, kept when a new run starts) and the new one to that URL, to see what happened on a
console without taking its SD card out (`source/logupload.c`), and that logs each second's
performance whether or not the bottom screen shows it. Any server that takes a POST will do.
Release builds leave all of it out: nothing in them uses the network.

## How the port works

Most of armGBA builds for the 3DS unchanged. The changes, each behind `__3DS__` or the ARM
architecture version unless said otherwise, so the other builds are untouched:

**Getting it to run**

- **The recompiler on ARMv6.** The 3DS's ARM11 MPCore is ARMv6K. The A32 backend was written for
  ARMv7, and of what it emits only three instructions are ARMv7's: MOVW and MOVT (32-bit constants)
  and UBFX (bit fields). `src/cpu/a32/emit.h` replaces them when built for ARMv6: a constant becomes
  MOV and up to three ORRs (or MVN and BICs, whichever is shorter), and a bit field two shifts, or
  one shift or an AND where that's enough.
- **Executable memory.** There's no `mmap` on the 3DS. `source/jitmem.c` takes the code memory
  from the heap (up to 24 MiB) and makes it executable with `svcControlProcessMemory` on Luma3DS.
  After code is written, one kernel routine cleans the data cache and invalidates the instruction
  cache for just the lines written: block links and patches are a word or two, thousands of times
  a second, and flushing whole ranges through the usual calls cost milliseconds a frame.
  Before the app ends the memory is made read-write again: a process left with executable memory
  hung the system on "Closing software".
- **Memory.** The app asks for the 80 MB mode (the 124 MB one was granted only on some launches).
  The core used to keep every game in a 32 MiB buffer, the whole cartridge space; the buffer is
  now the game's size (reads past it are checked), and the frontend reads or unzips a game
  straight into it and hands it to the core without a copy. The recompiler's bookkeeping takes
  about 0.4 bytes of heap per byte of code, so it also flushes when the heap runs low, not only
  when the code memory is full.
- **`-fwrapv`.** The core keeps its clocks in 32-bit counters that wrap after 2^31 cycles (128
  seconds of play) and compares them by subtracting, which needs signed overflow to wrap.
  devkitARM's GCC 16 optimized those comparisons away, and every game hung 128 seconds in.
- **Closing from the HOME Menu.** Once the system has asked the app to close, it no longer has the
  GPU: a frame submitted then never finishes. Nothing is drawn after that.
- **The Dolphin link's LAN search** skips `getifaddrs`, which the 3DS hasn't got.

**Getting it to full speed** (each measured on the console, through the development build's log)

- **Drawing on the other cores.** armGBA's Threaded Video logs the renderer's calls and replays
  them on another thread. On the New 3DS that thread runs on core 2, and a second one on core 3:
  each has its own copy of the renderer and video memory and replays the whole log, one drawing
  the even lines and the other the odd ones. A line a copy doesn't draw leaves its renderer as a
  line that hasn't changed does, so the picture is the same, pixel for pixel, as one renderer's
  (checked over thousands of frames in eight games). Mario Kart's races went from 7 to 11 ms of
  drawing a frame to 3.7 to 5.7 ms on each core, which is what let Threaded Video be On (each
  frame shown as soon as it's drawn) and not One Frame Late. This part isn't 3DS-only: any build
  can start a second worker (`ARMGBA_RENDER_WORKERS=2`).
- **Mixing the sound on another core.** Threaded Audio (On on the New 3DS) does for the mixer
  what Threaded Video does for the renderer: the game's thread keeps what a game can read back
  (the FIFOs and their DMA requests, the tone channels' lengths, sweep and envelopes, the status
  bits, channel 3's wave RAM) and logs each call that makes or changes samples; a thread on core 3,
  one priority step above the render worker it shares the core with, makes the same calls in the
  same order on its own copy. The samples, the picture and save states are the same byte for byte
  (eight games, 1,500 frames each). It takes about a tenth of the game's thread's instructions in
  a Mario Kart race and about 4% in a busy Advance Wars scene (counted on an ARM build of the
  core; not yet timed on a console: the development build's log has a `sound thread:` line).
- **Frame Skip's Auto only when frames are late.** It left frames out whenever the sound queue was
  a third full or less, which it also is for seconds after one slow frame (a game loading, a burst
  of compiling) while every frame since has time to spare: up to 24 frames in 60 were left out with
  the emulation needing 12 ms of the 16.7 (in Azahar). The frontend now reports a low queue only
  while frames miss their time. The development build's log counts the frames left out
  (`frame skip:`).
- **Code memory that doesn't fill up.** Stutter every few seconds turned out to be the code memory
  filling and everything being thrown away. Blocks are now laid out in stretches split between hot
  and cold code as the game's own code is, and a "careful copy" of a block (about three times its
  size) is made only once events have fallen inside it eight times.
- **New code is stepped through before it's translated.** Translating a block costs about as much
  as stepping through it a hundred times, and much of what a game runs while it loads runs once.
  A block is translated the third time it comes round; in a frame that has used up its spare time
  for translating, the fourth.
- **The warm list.** When a game is closed, the list of cartridge code that was translated is
  written to `warm/<game>.warm`. The next time, the app translates from that list in whatever is
  left of each frame's time, so the code is ready before the game reaches it. The list holds
  addresses only, no translated code: a wrong or stale one can only waste a little time.
- **The whole program compiled as one (LTO)**, since the core's hot paths cross its files: 8% less
  time in the Advance Wars scene above, 2.5% in a Mario Kart race.
- **Multiplies.** The A32 backend worked out the carry flag of every flag-setting multiply (every
  Thumb `MUL`) by calling into C; now only where something reads it, as the other backends do.

The frontend (`source/`) is a small libretro frontend: citro2d for the picture (each frame turned
into a texture by the GPU's transfer engine, then drawn scaled), ndsp for the sound, frames paced
to the GBA's own 59.73 Hz, and a text console on the bottom screen for the browser and menu. It
answers the core's options the way RetroArch does.

## Testing

The app runs scripts for testing without hands: `sdmc:/3ds/armgba/autotest.txt` (see the comment
at the top of `source/main.c`) loads a game or drives the browser, presses buttons, takes
screenshots of the core's frame and of both screens, saves and loads states, logs the frame rate,
and exits. The idea, and the blank DSP firmware handed to an emulator, come from
[MW3DS](https://github.com/3ds-conspiracy/mw3ds)'s Azahar test setup. Azahar doesn't time the
3DS's CPU as the hardware runs, so it shows whether something works, not how fast.

On a New 3DS: Mario Kart: Super Circuit and Advance Wars, played and measured as above.

In Azahar 2126.2:

| Test | Result |
| --- | --- |
| Recompiler against the interpreter: Pokémon Emerald, 1500 frames through the intro with button presses, 5 screenshots | All 5 frames identical, pixel for pixel |
| Pokémon Emerald, Golden Sun, Metroid Fusion, Mario Kart: Super Circuit, Advance Wars (`.zip`) | Boot and play into the game; screenshots correct |
| Pokémon Crystal (`.7z`, Game Boy Color on Gambatte) | Runs, 160x144 scaled to fit |
| Save states: save, then load in a later run | Works (Golden Sun, from the installed CIA) |
| Battery save, the whole way: Pokémon Emerald new game, saved in the game, app closed, started again with no save state | The `.srm` (128 KiB flash) is written, read back, and the title shows CONTINUE; it loads into the house where it was saved |
| Past 128 seconds of play | Runs (it hung before `-fwrapv`) |
| LCD3x | Centered at 720x240 on the 800-column screen; measured factors match lcd3x's |
| Two drawing cores, and the warm list | Run; Mario Kart's second run steps through less than half the untranslated code of its first |
| Browser, menu, settings, from a script | Work |

## Known gaps

- Tried on one console and closely on two games. Other games may be slow or wrong.
- **No link play of any kind.** The core's link options (link cable, Wireless Adapter, Four Player
  Adapter, GameCube link) aren't offered: none of it has been tried on a 3DS. Nor Game Boy
  Printer output or e-Reader cards.
- No rumble, gyro, tilt or solar sensor input; no cheats menu.
- No 3D (the picture is 2D on purpose).
- A Game Boy / Game Boy Color battery save hasn't been taken the whole way round yet (the GBA one
  has); they use the same frontend path.
- The original 3DS and 2DS are too slow.

## Credits and licence

armGBA is Magnominus's ([MagnominusGit/armGBA](https://github.com/MagnominusGit/armGBA)); see the
main README's Credits. This port was written by Anthropic's Claude (through Claude Code) at the
direction of the fork's owner, who tested it on a New 3DS. Like armGBA it is under the Mozilla
Public License 2.0. The frontend uses libctru, citro3d and citro2d (devkitPro, zlib licence) and
zlib. Thanks to [MW3DS](https://github.com/3ds-conspiracy/mw3ds) for showing how to test a 3DS app
in Azahar.
