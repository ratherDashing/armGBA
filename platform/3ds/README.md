# armGBA for the Nintendo 3DS

A port of armGBA to the 3DS: Game Boy Advance games on armGBA's own core, with its recompiler
rebuilt for the 3DS's ARM11, and Game Boy and Game Boy Color games on Gambatte, as in the libretro
core. It's a standalone app (a `.3dsx` for the Homebrew Launcher, or a `.cia` for the HOME Menu)
with a small frontend of its own: the game on the top screen, a menu on the bottom one.

> **Status: tested in the Azahar emulator only, not yet on a real 3DS.** Everything below that says
> "tested" was tested in Azahar 2126.2. Azahar doesn't time the 3DS's CPU the way the hardware runs,
> so how fast games run on a real New 3DS or Old 3DS is still unknown (see Performance). Reports
> from real hardware are welcome.

## What you need

- A 3DS, 2DS, New 3DS or New 2DS XL with **Luma3DS** (any modern custom firmware setup has it). The
  recompiler needs to make memory executable, which Luma3DS allows homebrew to do.
- For sound on real hardware: the DSP firmware dumped to `sdmc:/3ds/dspfirm.cdc` (run the *DSP1*
  homebrew once). Without it the games run silent.
- Your own games. armGBA comes with no games and downloads none.

## Install

- **Homebrew Launcher:** copy `armgba.3dsx` to `sdmc:/3ds/`.
- **HOME Menu:** install `armgba.cia` with FBI (FBI can also install it straight from a release
  link's QR code).

Put games anywhere on the SD card; the browser starts in `sdmc:/roms` and remembers the last folder.
It opens `.gba`, `.gb`, `.gbc`, `.bin` and `.agb` files, and the first game inside a `.zip` or
`.7z`, so a No-Intro set works as it is.

The app keeps its files in `sdmc:/3ds/armgba/`:

| Path | What |
| --- | --- |
| `system/` | Optional BIOS and boot ROMs: `gba_bios.bin`, and the Game Boy ones as the libretro core names them. Without them armGBA uses its own open-source ones |
| `saves/` | Game saves, `<game>.srm` and `.rtc`, the same files RetroArch makes: copy them either way |
| `states/` | Save states, `<game>.state` (slot 0) to `.state9` |
| `config.ini` | The screen settings and the core options that differ from the defaults |
| `log.txt` | What happened last run: attach it to a bug report |

## Controls

| 3DS | Game |
| --- | --- |
| A, B, L, R, Start, Select | The same buttons |
| D-pad or Circle Pad | D-pad |
| X, Y (held) | Turbo A, turbo B |
| ZL, ZR (held, New 3DS) | Fast-forward |
| Touch the bottom screen | The menu |

The menu has save and load state (ten slots), the screen size (pixel for pixel, fit, or stretched to
the whole top screen), smoothing, all of armGBA's core options, restart, and close game.

Two defaults differ from the libretro core, for the 3DS: **Skip BIOS Intro** is on, and **Frame
Skip** is Auto (the core skips drawing frames when the sound is about to run short, so the game keeps
full speed when the CPU can't).

## Building

With Docker, from the repository's root:

    docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src/platform/3ds devkitpro/devkitarm make -j4

That makes `platform/3ds/armgba.3dsx`. `make JIT=0` builds the interpreter alone (about 12 times
slower, but useful to check the recompiler against). `make cia` also makes `armgba.cia`, with
`bannertool` and `makerom` (from Project_CTR) on the `PATH`; makerom's release binaries need glibc
2.38 or newer, so run that step on the host or in a newer container. The GitHub workflow
(`.github/workflows/3ds.yml`) builds both.

## How the port works

Most of armGBA builds for the 3DS unchanged. The changes, each behind `__3DS__` or the ARM
architecture version so the other builds are untouched:

- **The recompiler on ARMv6.** The 3DS's ARM11 MPCore is ARMv6K. The A32 backend was written for
  ARMv7, and of what it emits only three instructions are ARMv7's: MOVW and MOVT (32-bit constants)
  and UBFX (bit fields). `src/cpu/a32/emit.h` replaces them when built for ARMv6: a constant becomes
  MOV and up to three ORRs (or MVN and BICs, whichever is shorter), and a bit field two shifts, or
  one shift or an AND where that's enough. Nothing in the backend patches a constant load in place
  (only branches are relinked), so the longer sequences are safe. BFI, which the backend doesn't
  use, aborts if anything ever does.
- **Executable memory.** There's no `mmap` on the 3DS. `platform/3ds/source/jitmem.c` allocates the
  code buffer (6 MiB, down from 16, since an Old 3DS has 64 MiB for everything) from the heap and
  makes it executable: with `svcControlProcessMemory` on Luma3DS, or Luma3DS's
  `svcControlProcess(PROCESSOP_SET_MMU_TO_RWX)`, which Azahar implements. After code is written,
  the data cache is written back (`svcFlushProcessDataCache`) and the instruction cache dropped for
  that range (Luma3DS's `svcInvalidateInstructionCacheRange`, 0x93, which Azahar implements too).
- **The cartridge buffer.** The core keeps a game in a 32 MiB buffer, the whole cartridge space, so
  out-of-range reads can't leave it. It used to copy the game into one, so a game took its size
  plus 32 MiB, too much for an Old 3DS. The frontend now reads or unzips a GBA game straight into a
  32 MiB buffer and offers it to the core (`gbaOfferRom`), which takes it over without copying.
  `gbaMapRom` (used when a game is opened by path) does the same instead of `mmap`.
- **The Dolphin link's LAN search** skips `getifaddrs`, which the 3DS hasn't got.

The frontend (`platform/3ds/source/`) is a small libretro frontend: citro2d for the picture (each
frame turned into a texture by the GPU's transfer engine, then drawn scaled), ndsp for the sound
(played at the core's rate scaled up by the screen's 59.83 Hz over the GBA's 59.73 Hz, so the
queue stays level when frames are paced by the screen), and a text console on the bottom screen
for the browser and menu. It answers the core's options the way RetroArch does, so every core
option works.

## Testing

The app runs scripts for testing without hands: `sdmc:/3ds/armgba/autotest.txt` (see the comment
at the top of `source/main.c`) loads a game or drives the browser, presses buttons, takes
screenshots of the core's frame and of both screens, saves and loads states, logs the frame rate,
and exits. The idea, and the blank DSP firmware handed to an emulator, come from
[MW3DS](https://github.com/3ds-conspiracy/mw3ds)'s Azahar test setup.

Tested in Azahar 2126.2 (New 3DS and Old 3DS modes):

| Test | Result |
| --- | --- |
| Recompiler against the interpreter: Pokémon Emerald, 1500 frames through the intro with button presses, 5 screenshots | All 5 frames identical, pixel for pixel |
| Pokémon Emerald, Golden Sun, Metroid Fusion, Mario Kart: Super Circuit (`.zip`) | Boot and play into the game; screenshots correct |
| Pokémon Crystal (`.7z`, Game Boy Color on Gambatte) | Runs, 160x144 scaled to fit |
| Old 3DS mode (64 MiB): Golden Sun | Runs; the recompiler's memory and the 32 MiB cartridge buffer fit |
| Save states: save, then load in a later run | Works (Golden Sun, from the installed CIA) |
| Game saves | Read at start, written when the game changes them and on exit |
| Browser, menu, core options, from a script | Work |
| The CIA, installed with Azahar's `-i` | Runs |

## Performance

Unknown on real hardware for now. In Azahar, the recompiler ran the GBA games at 75 to 150 frames a
second of emulation alone (the core without waiting for the screen), and the interpreter at 6, but
Azahar runs the 3DS's CPU at its own pace, not the hardware's: those numbers only compare one build
with another. armGBA's recompiler was made for chips like the Cortex-A35 at 1.2 GHz and up; the New
3DS's ARM11 at 804 MHz is slower, and the Old 3DS's at 268 MHz much slower. The New 3DS's speed-up
(804 MHz and the L2 cache) is turned on. If games run slow, try in the core options: **Frame Skip**
(Auto is on), **CPU Clock** below 100% (eases slowdown in the game itself), and leave **CPU LCD
Effects** and **GBA Color Correction** off.

## Known gaps

- Not run on real hardware yet.
- No link cable, netplay, GameCube link, Game Boy Printer output or e-Reader cards: the frontend
  doesn't offer the core's netplay or disk control.
- **Threaded Video** isn't set up to use the 3DS's other CPU cores, so it may not help; untested.
- No rumble, gyro, tilt or solar sensor input; no cheats menu.
- No 3D (the picture is 2D on purpose).

## Credits

This port was written by Anthropic's Claude (through Claude Code) at the direction of the fork's
owner, and tested in Azahar as described above. armGBA itself is Magnominus's; see the main
README's Credits. The 3DS frontend uses libctru, citro3d and citro2d (devkitPro, zlib license) and
zlib. Thanks to [MW3DS](https://github.com/3ds-conspiracy/mw3ds) for showing how to test a 3DS app
in Azahar.
