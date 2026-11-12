# armGBA

A Game Boy Advance, Game Boy and Game Boy Color libretro core for low-power handhelds.

armGBA builds on other projects. Parts of its Game Boy Advance side come from mGBA (Jeffrey Pfau and
contributors), notably the renderer, cheat engine, sound channels and save chips, and mGBA is the
reference it's tested against. Game Boy and Game Boy Color games run on Gambatte-Speedrun's core.
See Credits for the full list.

armGBA adds a recompiler for the Game Boy Advance's CPU, so games run with headroom on slow handheld
chips such as the Cortex-A35 and Cortex-A55. It stays accurate wherever a game could notice:
interrupts, DMA, timers and what's on screen all happen when they would on the hardware.

## Legal

- **Your own games.** armGBA comes with no games and doesn't download any. Play only games you own,
  dumped from your own cartridges or bought legally. armGBA doesn't support or promote piracy.
- **Nothing is shared in multiplayer.** Netplay never sends a game, a BIOS or a boot ROM; each
  player uses their own files.
  - In Together and Off, every device runs every player's console, so every player needs their own
    legally owned copy of every game in the session, including player 2's game when it differs.
  - A host on Nintendo's BIOS or boot ROM needs joiners with their own copy of the same file.
    armGBA's open-source BIOS and boot ROMs are built in, so every device has those.
  - In Separate, only what the game itself sends over the emulated link cable crosses the network,
    such as a single-pak game's program, as on the real hardware.
- **Not Nintendo.** armGBA is an independent project, not affiliated with or endorsed by Nintendo.
  Game Boy, Game Boy Pocket, Game Boy Light, Game Boy Color and Game Boy Advance are trademarks of
  Nintendo, and the consoles' BIOS and boot ROMs are Nintendo's copyright.

## Controls

| Button | Game Boy Advance | Game Boy, Game Boy Color |
| --- | --- | --- |
| L2, R2 (held) | Fast-forward, up to RetroArch's Maximum Run Speed | Same |
| X, Y (held) | Turbo A, turbo B | Same |
| L3, R3 (held) | Turbo L, turbo R | Clicking the tilt stick flicks (motion sensor); R3 otherwise sets the contrast back to 50% |
| L, R | L, R | Stretch the picture (Game Boy Advance model) |
| Tilt stick | Tilt and turn (motion sensor games) | Tilt (motion sensor games) |
| Left stick, with Left Stick as D-Pad | D-pad, except while it tilts | Same |
| Right stick up, down | Sunlight up, down (solar sensor games) | Contrast up, down (original Game Boy screens) |
| L, R, Start (held 1 s) | Restart that player's console (the Restart Combo option) | Same |

The tilt stick is the left stick by default. Tilt Control moves it to the right stick or the D-pad;
with the D-pad, L is the flick. Device Motion tilts the cartridge with the device itself, where the
frontend passes its motion sensors on (RetroArch on Android, and the standalone frontend on devices
with them): the cartridge feels what the device does, held flat reading level, and a quick flick
counts as one. L3 also flicks, and the gyro cartridge turns with the device. In netplay, or with no
readings, the left stick tilts instead. If tilting goes the wrong way, Device Motion Sensor Rotation
turns the sensor to match the screen: usually 0° in the standalone app, often 90° in RetroArch.

With the consoles of a link on one device, R2 changes how the screens are shown. L2 fast-forwards or
changes the layout back, as the L2 Button option says; in netplay, L2 always changes it back. Each
device in netplay shows the screens its own way, changed by its own player's R2. RetroArch 1.22.2
and older don't tell the core which player is on a device, so there the host follows player 1 and
other devices follow player 2, unless Screen Layout Control names the player. Show Player Numbers
marks each player's screen.

## Options

The menu shows only the options that do something for the game loaded and the Link Mode chosen. The
Tilt and Solar Sensor options and the Clock options appear only for cartridges that have one, and
the link options only with Link Mode on. There are seven categories. Link's options are split into
four parts (Link, Link: Players, Link: Connection, and Link: Screens and Controllers), which the
standalone frontend shows as one page with headers.

| Category | Option | Console | Values (default first) |
| --- | --- | --- | --- |
| System | Skip BIOS Intro | Game Boy Advance | Off, On |
| | BIOS (applies on restart) | Game Boy Advance | Nintendo's (System Folder), armGBA's (Built In) |
| | CPU Clock | Both | 100%, 25% to 400% in steps of 25% |
| | CPU Clock During Boot | Both | 100%, CPU Clock (with CPU Clock not at 100%) |
| | Low Memory (applies when a game is opened) | Game Boy Advance | Off, On |
| | Game Boy Model (applies on restart) | Game Boy | Auto, Game Boy, Game Boy Pocket / Light, Game Boy Color, Game Boy Advance |
| | Game Boy Boot ROM (applies on restart) | Game Boy | Nintendo's (System Folder), armGBA's (Built In), Off |
| Clock | Clock Source | Both | Real Time, Game Time |
| | Clock Speed (with Game Time) | Both | Stopped, 0.1x, 0.25x, 0.5x, 0.75x, 1x, 1.25x, 1.5x, 2x, 3x, 4x, 6x, 10x, 24x, 60x (1x by default) |
| | Clock Offset: Days | Both (not HuC3) | -30 to +30 days, 0 by default |
| | Clock Offset: Hours | Both (not HuC3) | -23 to +23 hours, 0 by default |
| | Allow Clock Reset | Both (Game Boy: MBC3) | Off, On (back to Off after a reset) |
| | Reset Clock (with Allow Clock Reset on) | Both (Game Boy: MBC3) | Off, Reset |
| Video | Threaded Video | Game Boy Advance | Off, On, One Frame Late |
| | Video Threads (with Threaded Video) | Game Boy Advance | 1, 2 |
| | GBA Color Correction | Game Boy Advance | Off, On |
| | Game Boy Color Correction | Game Boy | Off, On |
| | Game Boy Screen | Game Boy | Grayscale, Game Boy, Game Boy Pocket (both measured from real ones), Game Boy Light (Backlit) |
| | Game Boy Contrast | Game Boy | 0% to 100% of the wheel in tenths, 50% by default (original Game Boy screens) |
| | Game Boy on GBA: Border | Game Boy | Cropped, Shown (Game Boy Advance model) |
| | Game Boy on GBA: Screen | Game Boy | Game Boy Advance, Game Boy Color (Game Boy Advance model) |
| | Frame Skip | Both | Off, Auto, 1, 2, 3 (not in netplay) |
| | Fast-Forward Skip | Game Boy Advance | Off, On (on by default; not in netplay) |
| Video: LCD Effects | CPU LCD Effects | Both | Off, On (each effect below keeps its setting) |
| | LCD Ghosting | Both | Off, 25%, 50%, 75%, 100% |
| | Interlaced Rows | Both | Off, On (Color's and Advance's screens) |
| | LCD Scale | Both | Off, Fit Screen, 2x to 8x in steps of 0.5x |
| | Screen Size (with Fit Screen) | Both | Auto, 480x320 to 3840x2160 (23 sizes), Custom |
| | Custom Width, Custom Height (with Custom) | Both | Hundreds (100 to 4000), tens (+0 to +90) and even ones (+0 to +8), 1280x720 by default |
| | Pixel Gaps | Both | Off, On (with LCD Scale) |
| | LCD Subpixels | Both | Off, 10% to 100% in tenths (Color's and Advance's screens, with LCD Scale) |
| | Game Boy Pixel Shadow | Game Boy | Off, Up, Up Right, Right, Down Right, Down, Down Left, Left, Up Left (with LCD Scale); in the app's GPU mode also Tilt (Android: follows the device's tilt) |
| | Game Boy Pixel Shadow Distance | Game Boy | 0.5 to 2.5 dots in halves, 1.5 by default; how far Tilt reaches in the app |
| | LCD Effect Threads | Both | Auto, 1, 2, 3, 4 |
| Audio | Threaded Audio | Game Boy Advance | Off, On (the sound mixed on another CPU core; the same samples) |
| | Low-Pass Filter | Both | Off, 20%, 40%, 60%, 80% |
| Input | Left Stick as D-Pad | Both | Off, On |
| | Turbo Button Period | Both | 4 to 120 frames, 4 by default |
| | Tilt Control | Both | Left Stick, Right Stick, D-Pad, Device Motion |
| | Device Motion Sensor Rotation (with Device Motion) | Both | 0°, 90°, 180°, 270° |
| | Solar Sensor | Game Boy Advance | 0 (Dark) to 10 (Full Sun), 5 by default |
| | Restart Combo | Both | L + R + Start, L + R + A + B, L3 + R3, Select + Start + A + B, Off |
| Link | Link Mode (applies on restart) | Both | Off, Together, Separate, GameCube (Game Boy Advance) |
| | Shared Controls (Link Mode Off) | Both | Off, Everyone (every player's buttons at once), Pass Around (one player at a time, taking over with the Switch Controller Combo) |
| Link: Players | Number of Players (applies on restart) | Both | 2, 3, 4 (Together) |
| | Player 2's Game (applies on restart) | Both | Same Game, then the games beside player 1's (Together) |
| Link: Connection | Cartridge (applies on restart) | Game Boy Advance | Inserted, Removed (Separate, GameCube) |
| | GBA Link Port (applies on restart) | Game Boy Advance | Link Cable, Wireless Adapter |
| | Game Boy Link Port (applies on restart) | Game Boy | Link Cable, Four Player Adapter (Together) |
| | Game Boy Link Timing | Game Boy | Standard, Exact (Separate) |
| | Game Boy Printer | Game Boy | Off, On (Link Mode Off) |
| | Dolphin Address | Game Boy Advance | Find on the Network, This Device (GameCube) |
| Link: Screens and Controllers | Screen Layout | Both | Side by Side, Top and Bottom, Player 1 Only to Player 4 Only (Together) |
| | L2 Button | Both | Fast-Forward, Previous Layout (Together; outside netplay) |
| | Screen Layout Control | Both | Auto, Player 1 to Player 4 (Together) |
| | Switch Controller Combo | Both | Start + L2 + R2, L + R + A + B, L3 + R3, Start + L3, Start + R3, Off; while Start is held, L2 and R2 only switch, and the stick clicks it uses always do |
| | Show Player Numbers | Both | Off, On (default) (Together) |
| Saves | Back Up Save | Both | Off, then each player |
| | Load Save | Both | Off, then each player's current save (with two or more) and backups |
| | Load Save Into | Both | Same Player, then each player (Together) |
| | Send Save | Both | Off, then the saves found, newest first, with their times (Save Manager, with no game) |
| | Receive Saves Into | Both | Auto (default), Your Save, Player 2's to Player 4's Save (Save Manager) |
| Saves: Settings | Keep My Netplay Progress | Both | Off (default), On |
| | New Players' Saves (applies on restart) | Both | Create New Save, Copy Player 1's Save (Together) |
| | Player 1's Save Folder (applies when a game starts) | Both | Save Folder, armGBA/Player 1 Folder |
| | Backups Kept | Both | 10, 5, 20, All |
| | Backups Listed | Both | 10, 20, 40, 80, 126 |
| | Safe Saves | Both | Off, On; On by default |

### CPU Clock

CPU Clock runs the CPU faster or slower than the rest of the console. On the Game Boy Advance, the
CPU, its bus and DMA move to the new clock, as with MAME's CPU overclock. On the Game Boy and Game
Boy Color, the CPU and OAM DMA do. The screen, timers and sound keep the console's clock, so games
run at their normal speed, and games that slow down or draw fewer frames than the screen shows, such
as 3D ones, run smoother. A faster clock uses more of the host's CPU while a game is busy.

- Game Boy Color games in double speed go no faster than 200%, four times the first Game Boy's CPU.
- CPU Clock During Boot at 100% keeps the BIOS intro and boot ROMs as they are on the console.
- On the Game Boy and Game Boy Color, the CPU runs at the console's own pace for two lines after a
  game reads STAT or LY, so code timed against the picture still lines up.
- A few games depend on the CPU's speed in their own logic and misbehave above 100%, as they would
  on an overclocked console. Set CPU Clock back to 100% for those.

### Low Memory

With Low Memory on, RetroArch leaves loading the game to the core, which maps the file into memory
instead of keeping a copy. The host then reads only the parts of the game in use and can drop them
again. Games with a soft patch beside them, and files over 32 MiB, load as they do with Low Memory
off. It works best with unzipped games: RetroArch unpacks a zipped game to a temporary folder away
from its soft patch, so the game runs unpatched. The standalone frontend always gives the core the
game itself, so it doesn't show Low Memory.

### Frame Skip

Frame Skip leaves frames out of the picture when the host can't keep up. Auto skips while
RetroArch's sound buffer is a third full or less, never more than three frames in a row. 1, 2 or 3
skip that many frames after each frame shown. The game still runs every frame, so it plays, sounds
and saves the same. A skipped frame isn't drawn by the Game Boy Advance's renderer or by the LCD
effects, and RetroArch shows the last picture again. LCD Ghosting catches up on the next frame
shown. Frame Skip stays off in netplay, where every device's state must match.

Fast-Forward Skip does the same for the frames a frontend runs but won't show while it
fast-forwards: RetroArch with its fast-forward frame skip on, and the standalone frontend, which
shows only the last frame before each picture. It's on by default and works for Game Boy Advance
games; it stays off in netplay.

### Game Boy games on the Game Boy Advance model

On the Game Boy Advance model, a Game Boy game is shown at its own size. Game Boy on GBA: Border set
to Shown also shows the black around it, as that console's screen does. Game Boy on GBA: Screen
picks the panel it's shown on, the Advance's or the Color's, which decides its colors (with color
correction on), its LCD grid and its ghosting.

### LCD effects

CPU LCD Effects turns every effect the core draws on the CPU off or on at once, and each effect
keeps its own setting. The effects work independently, except that Pixel Gaps, LCD Subpixels and the
Game Boy pixel shadow draw inside each pixel, so they need LCD Scale's larger picture and show only
with it.

Interlaced Rows, for the Color's and Advance's panels, updates every other row each frame. A picture
that changes every frame then shows in stripes, as it does on those consoles. On its own, the rows
not updated keep what they showed. With LCD Ghosting on as well, every row fades, and the updated
rows fade faster.

LCD Scale draws the picture 2x to 8x larger, with room inside each pixel for Pixel Gaps (the gaps
between the screen's pixels) and for the shadow that the original Game Boy's and the Pocket's pixels
cast. Pick the largest size that fits the screen, with RetroArch's integer scaling on. At a half
step such as 2.5x, each 2x2 block of pixels becomes a whole number of output pixels (5x5), so the
gaps repeat every two pixels. On a 640x480 handheld, 2.5x (600x400) fills most of the screen with a
Game Boy Advance game.

Fit Screen draws the picture as large as fits a screen size, to the output pixel, so the frontend
shows it pixel for pixel with no stretching: on a 1240x1080 screen a Game Boy Advance game is drawn
at 1240x827. RetroArch doesn't tell a core the screen's resolution, so Screen Size says it: Auto
takes the standalone frontend's window size, else reads the display's where the system gives it
(Linux's DRM, as on ROCKNIX), else 1280x720; a listed size; or Custom, set as hundreds, tens and
even ones, to the pixel. At such a size no block of pixels repeats, so each output column and row
has its own source pixel, share of gap and stripes, worked out once for the size. Use it with
RetroArch's aspect ratio set to the core's and integer scaling either way. Stretched by RetroArch
instead, a picture's columns land unevenly, and stripes and gaps show as uneven bands.

LCD Scale runs on the CPU in vector code (NEON, SSE or AVX). LCD Scale and LCD Ghosting split their
rows among the CPU's cores. LCD Effect Threads sets how many cores; Auto leaves one free, and one
more for each of Threaded Video's threads. The shadow is one shade for each pixel's square, close to
the smooth shadow that the shaders and the standalone frontend draw, at a fraction of the cost. At
6x, a Game Boy game's grid adds about 1.5 ms a frame on an RG353M and its shadow 0.1 ms more; on a
Core i7-9700F, 0.07 ms and 0.02 ms.

LCD Subpixels draws the red, green and blue stripes of the Color's and Advance's screens inside each
pixel. Photos of the two screens show red, green and blue from the left on the Color, and blue,
green and red on the Advance. The stripes are mixed over each pixel's color in linear light, as
`(1 - mix) * color + mix * stripes`, with each stripe lighting only its own channel. At 100% a pixel
is its stripes alone, a third as bright.

The stripes are laid across the output columns each pixel covers, in as many bands as the narrowest
pixel has columns. From 3x at whole sizes, each stripe is a column or more. At 2x there are two
bands, one cyan-ish and one orange-ish. At 2.5x, pixels alternate between two and three columns:
every pixel gets the same two bands, and the three-column pixels blend them in the middle column, so
neighbors match and each pixel still averages to its own color. The stripes fade in as a dot grows
from one output pixel to two. At 100% they add about 0.25 ms a frame to the core's grid on one
Raspberry Pi 5 core, at 3x or 6x. With Pixel Gaps on, the brightness keeping is worked out for each
channel through its own stripes: the blue and red stripes sit at a pixel's edges, under the gaps,
and the green one between them, so each channel is made up for as far as its own stripe is covered
and the colors stay true.

RetroArch's shader presets in `shaders/` draw every one of these effects on the GPU instead, at any
size and without integer scaling, so turn CPU LCD Effects off when using them. There is one preset
for each screen, `.slangp` and `.glslp`: `gb`, `gb-pocket`, `gb-light` and `gb-gray` (with the
shadow), `gbc` and `gba`. Each runs two passes. The first draws LCD Ghosting and Interlaced Rows at
the console's resolution from the last frame shown, with the same math as the CPU and each screen's
times; Interlaced Rows may fall on the other set of rows, as RetroArch counts frames its own way.
LCD Ghosting is at 100% in every preset; its shader parameter turns it down or off.
The second draws the grid, with Pixel Gaps, Subpixels (and Subpixel Order) and the pixel shadow.

For slower GPUs, the `.glslp` `*-fast` presets use `armgba-lcd-fade-fast.glsl` and
`armgba-lcd-fast.glsl`. They keep the same screen colors, gaps and ghosting controls, but approximate
ghosting directly in gamma-encoded color, use gamma-encoded brightness compensation, and omit pixel
shadows and subpixels to reduce per-pixel work and texture reads. Their grid uses fractional gap
coverage to smooth lines at non-integer scales, and compensates brightness using average gap coverage.
They are a lighter visual approximation, not a pixel-identical substitute.

The full GPU grid puts each gap on the output pixel edge where one pixel gives way to the next, as
Fit Screen does, so every gap is drawn alike at any size. Both grids are at full strength from 3x,
and fade out towards 1x, where a gap is under a pixel wide.

The standalone frontend draws the same effects on the GPU itself, with the same shaders. Its LCD
Effects setting, in place of CPU LCD Effects, is Off, CPU (the core draws every effect), GPU (the
app draws every effect) or GPU Fast (the `-fast` presets' shaders, without subpixels or the pixel
shadow), never some of each, and each effect keeps its setting. On the GPU, LCD
Ghosting moves on once for each picture the core sends, and Pixel Gaps and the pixel shadow are set
with the other LCD effects in Core > Video. The GPU needs SDL's GL or GLES renderer; with its
software renderer, LCD Effects is Off or CPU.

## Building

    cmake -S . -B build -G Ninja
    ninja -C build

This makes `armgba_libretro.so`. `libretro/armgba_libretro.info` goes in RetroArch's info directory.

The recompiler has backends for x86-64, AArch64 and 32-bit ARM (ARMv7 with NEON). For a 32-bit ARM
device such as the Miyoo Mini, cross-compile with that device's own toolchain file, which sets
`CMAKE_SYSTEM_PROCESSOR` to `arm`, and the ARMv7 flags:

    cmake -S . -B build-armv7 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<toolchain file> \
        -DCMAKE_C_FLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard" \
        -DCMAKE_CXX_FLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard"
    ninja -C build-armv7

On any other CPU, or with `-DARMGBA_JIT=OFF`, the exact CPU runs everything.

For RetroArch on Android, build with the NDK for `arm64-v8a`, `armeabi-v7a` or `x86_64`. The core is
then named `armgba_libretro_android.so`:

    cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
    cmake --build build-android

## Nintendo 3DS

This fork adds **armGBA-3DS**, a port to the New Nintendo 3DS, with the recompiler on the 3DS's
ARMv6 CPU and the picture drawn on its two spare cores: a `.cia` for the HOME Menu and a `.3dsx`
for the Homebrew Launcher, built with devkitARM. It's an alpha: full speed in the games measured
on a New 3DS so far. See [platform/3ds/README.md](platform/3ds/README.md), and the releases named
`3ds-v...` for downloads.

## Standalone frontend

`armgba`, armGBA's own frontend, runs a game in a window with SDL2, without RetroArch. It is
Armulators' app (the `armulators` submodule), which armN64 shares. armGBA gives it its names and
systems (`app.cmake`), and the Game Boy screens' LCD effects and the netplay rules (`app/`):

    git submodule update --init
    cmake -S . -B build -G Ninja -DARMGBA_SDL=ON
    ninja -C build armgba-sdl

`armulators/package/appimage/build.sh`, then `package.sh` beside it, make AppImages for x86-64 and
64-bit ARM in a Debian 11 container (Docker), with SDL2 built in. They need only glibc 2.29 or
later. `armulators/package/android/build.sh` makes the Android APK. They put their output in
`build-appimage/` and `build-android/`.

### Running and controls

Run `armgba game.gba` (or a `.gb`, `.gbc`, `.zip` or `.7z` file), or drop a game on the window. Esc,
a controller's Hotkey + X or a right-click opens the menu. The menu changes all of the controls
below, the Hotkey included.

| Keyboard | Does |
| --- | --- |
| Arrow keys | D-pad |
| X, Z | A, B |
| A, S | L, R |
| Enter, Backspace | Start, Select |
| C, V | Turbo B, turbo A |
| Tab (held) | Fast-forward |
| R | Rewind |
| P | Pause |
| F2 | Restart |
| F5, F7 | Save, load a state |
| F6, F8 | Change the state slot |
| F3 | Show the frame rate |
| F9 | Start or stop recording |
| F11 | Full screen |
| F12 | Screenshot |

Controllers work too, one per player. Their hotkeys are ROCKNIX's, held with the Hotkey (Home, or
Select on a controller without one):

| Hotkey + | Does |
| --- | --- |
| Start (pressed twice) | Quit |
| A | Pause |
| R, L | Save, load a state |
| R2 | Fast-forward on or off |
| L2 | Rewind |
| X | Menu |
| Y | Show the frame rate |
| B | Screenshot, as many RetroArch setups have it |

While the game is paused, fast-forwarding or rewinding, a symbol shows at the top right.

A controller's face buttons go by position, as RetroArch's RetroPad and a Game Boy have them: A on
the right, B at the bottom, X at the top, Y on the left. Controls' Swap A/B and X/Y puts A at the
bottom and B on the right on every controller. The control settings name each button as it's printed
on the controller, along with where it is.

### Menu

The menu has four tabs, which L and R move between (Q and E, or Tab, on the keyboard; left and right
also work on a row without a value). It opens again where it was left.

- **General:** the recent games, Load Game, the app's settings and Quit. The settings cover Video
  (window, scaling, vsync, the filter, and the frame rate and messages on screen), the sound's
  volume and latency, Controls, Fast-Forward and Rewind (with Run-Ahead), Saves and Folders, the
  menus' look (accent color, size, background), and Reset All Settings. The filter is Sharp,
  Smooth, or Sharp Smooth, which scales by whole pixels and blends only their edges, so pixels stay
  even on a screen the game doesn't fill a whole number of times.
  - Controls has Players, which picks the controller for each player, as RetroArch's Device Index
    does, and remembers it for when the controller connects again. A ROCKNIX handheld's own controls
    show as Built-In Controls.
  - Saves and Folders has Auto Save State and the folders for games, BIOS files, saves, states and
    screenshots. The Games folder is found by itself: ROCKNIX's roms folder, or a roms folder
    holding gb, gbc or gba. Load Game opens there, with those three first.
- **Game:** the loaded game's state slots (0 to 99, each with a picture) and Undo Load State, its
  cheats (RetroArch's `.cht` files can be added), its saves (the core's Saves options: backing up,
  loading and sending), Restart, Take Screenshot, recording and Close Game.
- **Core:** the core's options, in RetroArch's categories, with LCD Effects (Off, CPU, GPU or GPU Fast) in
  Video in place of CPU LCD Effects.
- **Extras:** netplay and RetroAchievements.

Rows ending in "..." open a page. Rows with a value step through it, except Back Up Save, Load Save
and Send Save, which act once something is chosen from their list. The line under the menu says what
the selected row does. With a controller, the buttons bound to A and B choose and go back, and in
the game list L and R jump by letter; on the keyboard, typing the start of a name finds it. Restart,
Close Game, Quit (with a game running), Reset All Settings and Reset Controls and Hotkeys ask for a
second press, as the bottom left says.

### Netplay

The frontend's netplay connects armgba to armgba. Its page puts hosting and joining first and its
settings last, as RetroArch's does. The settings include Link Mode and Shared Controls, as on Core's
Link page, and Keep My Netplay Progress, as on Core's Saves page. Create a Host Session links the
devices as Link Mode says:

- **Off:** one console that every player shares, as Shared Controls says (everyone's buttons at once
  if Shared Controls is off), for up to four players.
- **Together:** every device runs every player's console, kept in step with rollback.
- **Separate:** the link cable runs between two devices, each running its own game.

In Off and Together, each controller on a device is a player, in order, up to the session's number
of players. In Separate, each device is one player.

Off and Together need no game loaded to join. The host's game loads from your folders by its file
name, if your file is the same as the host's. armgba looks beside your game, in the game list's
folder, in the Game Boy, Game Boy Color and Game Boy Advance folders, and in the Games folder two
levels down. If no file by that name is found, the game list opens so you can pick your copy.
Separate needs your own game, or Your Cartridge: Removed.

Join's Scan for Local Hosts lists the sessions on the local network below it, with each host's Link
Mode and players. It finds them by a broadcast on the netplay port and updates the list while it's
shown; choosing it again scans afresh. Join a Host... takes an address. During a session the page
shows the players by name, and on the host its own address, for Join a Host... on another device.

In Together, a player who joined can Send My Save: their own save goes into their console, which
restarts, and the host backs up the save it replaces. With Keep My Netplay Progress on, their
progress becomes their save as they leave.

A device that joined Off or Together plays on when the session ends, whether it left or the host
did, with the controller still on its player. The game it ran was the host's, so it keeps no state
of it. In RetroArch the core does the same when RetroArch's netplay ends (in RetroArch newer than
1.22.2, as ROCKNIX ships). Either way, the session's saves are kept in armGBA/Remote.

Start Save Manager closes the game for Save Manager (see Backups below). From the command line,
`--host` (as Link Mode says), `--host-off`, `--host-together`, `--host-separate` and `--connect
ADDRESS` start a session, and `--host --no-cartridge` starts hosting Save Manager. Achievements need
the system's libcurl, and recording needs its ffmpeg.

### Files

The frontend's settings are in `~/.config/armgba/frontend.cfg`, the core's options in
`~/.config/armgba/armgba.cfg` and the recent games in `~/.config/armgba/recent.txt`. BIOS and boot
ROM files go in `~/.config/armgba/system`, and saves, states, screenshots and cheats in
`~/.local/share/armgba`.

On ROCKNIX, the frontend follows ROCKNIX's other standalone emulators: BIOS files come from
`roms/bios`, saves sit beside the games, states go in `roms/savestates` by console (`gba`, `gb`,
`gbc`) and screenshots in `roms/screenshots`. General's Saves and Folders changes each of these to
armGBA's own folder, a folder you type in, or, for saves, In Game Folder, beside each game. The app
never reads RetroArch's settings.

## BIOS and boot ROMs

Game Boy Advance games run on Nintendo's `gba_bios.bin` from the system directory when it's there
and BIOS is set to Nintendo's. Otherwise they run on armGBA's own open-source BIOS, built into the
core. Its splash lasts as long as Nintendo's logo, and a game starts on the same cycle.

Game Boy boot ROMs are optional, looked for under these names in this order:

- Game Boy: `gb_bios.bin`, `dmg_boot.bin`
- Game Boy Pocket: `mgb_boot.bin`
- Game Boy Color: `gbc_bios.bin`, `cgb_boot.bin`
- Game Boy Advance: `cgb_agb_boot.bin`, `cgb_agb0_boot.bin`, `agb_boot.bin`, then the Game Boy
  Color's (converted)

With Game Boy Boot ROM set to Nintendo's, the boot ROM is used under whichever of these names it
has. A SameBoy boot ROM under one of them is used only where Nintendo's isn't there. With neither,
armGBA's own starts the game.

armGBA's own boot ROMs (`gbboot/`, built into the core) are the same size as Nintendo's and run on
the hardware. On the Game Boy and the Pocket, in 256 bytes, the cartridge's logo scrolls down as it
does on Nintendo's. On the Game Boy Color and Game Boy Advance, in 2,304 bytes, "ARMGBC" comes in
over the cartridge's logo where Nintendo's title would. They take as long as Nintendo's, to the
cycle, and leave the machine as Nintendo's do. They give original Game Boy games on a Game Boy Color
the same colors, chosen the same ways. All of this was worked out from what Nintendo's boot ROMs
leave and show (`tests/gbcheck/`), never from their code.

## Features

### Cheats and achievements

The frontend's cheat codes work on both consoles. For the Game Boy Advance, GameShark, Action Replay
and CodeBreaker codes work, encrypted or not; they are decoded as mGBA decodes them and applied as
each frame ends. For the Game Boy, Game Genie and GameShark codes work. The memory is described to
the frontend as mGBA's and gambatte-libretro's cores describe it, so achievements read the same
addresses.

### Saves

The save type is detected on first use, with mGBA's override list. Save files are interchangeable
with mGBA's core. An EEPROM reports busy for about 7 ms after a write, and Flash for 1.8 ms after a
sector erase, as in mGBA.

### Clocks

A Game Boy cartridge's clock (MBC3 or HuC3) is kept in the `.rtc` file and moves on by the time the
game was off. gambatte-libretro and mGBA read the same file: gambatte-libretro its first 8 bytes,
and mGBA the MBC3 registers after them. armGBA reads either core's own file too. A host clock that
was never set, or that went back, adds no time. While a game runs, its clock follows the device's
time, so fast-forward and pauses don't move it, and a loaded state leaves an MBC3 clock as it is.

A Game Boy Advance cartridge's clock reads the device's time. The chip only counts from 2000 to
2099, so a host clock that was never set, which starts at 1970, reads as 2000 and counts on from
there. mGBA and gpSP keep no Game Boy Advance `.rtc` file, and the save file is the same in all
three.

With Clock Source set to Game Time, the game has a clock of its own. It runs only while the game
runs, at Clock Speed, and a Game Boy Advance game keeps it in a `.rtc` file too, starting from the
device's time. A state loaded in Game Time brings its clock with it.

The Clock Offsets move the clock forwards or back. A Game Boy clock stops at day 0, and moving
forwards again first makes up what it couldn't go back. A game that counts days from its last save
may hold its daily events after the clock goes back, until the clock catches up.

Reset Clock appears only once Allow Clock Reset is on, and Allow Clock Reset goes back to Off after
the reset, so a reset takes two steps.

- On a Game Boy cartridge, it clears the MBC3 clock with the halt bit set, as a dead battery leaves
  it. Games that check ask for the time again as they start. The save is kept.
- On a Game Boy Advance cartridge, it puts the clock back on the device's time: the Clock Offsets go
  back to 0, and Game Time starts again from now. It never sets the clock's power-loss flag, which
  games report as a dead battery.

In netplay, a cartridge's clock counts the console's time from the host's, so every device reads
the same. Every player needs the same Clock options.

### Game Boy Advance cartridge hardware

armGBA has the Matrix mapper of the 64 MiB Game Boy Advance Video cartridges, the bank register of
multi-game cartridges, Vast Fame's protection, and the mirrored ROM of the Classic NES Series. It
also has, as mGBA does, the light sensor, gyro, tilt sensor and rumble motor some cartridges carry.
A stick moves them the way mGBA's SDL frontend scales a stick.

The e-Reader reads cards as mGBA's does: a card's dot code is drawn as the strip would show it, and
the e-Reader's own program decodes it. Cards are added through RetroArch's Disc Control (Load New
Disc), or the standalone frontend's Game tab (e-Reader Cards). A card can be a `.raw` dump (with its
error-correcting bytes) or a `.bin` one (without them), and a file holding several strips is scanned
a strip at a time. A card waits until the e-Reader scans.

### Game Boy and Game Boy Color

These run on Gambatte-Speedrun's core (`gambatte/`), plus mappers for unlicensed cartridges: Sachen,
TAMA5, MBC6, MBC7, Vast Fame, and the BBD, Hitek, Sintax, Li Cheng, NT and Makon families.

- The Game Boy and Game Boy Pocket screens are BGB's measurements of real ones
  (bgb.bircd.org/reality).
- The Light's screen is the Pocket's lit by a cyan backlight, with its color and contrast taken from
  photos of real ones.
- Grayscale is gambatte-libretro's.
- The Game Boy Color and Game Boy Advance color correction is SameBoy's.
- The built-in boot ROMs are armGBA's own.

The Game Boy Printer (Game Boy Printer option, Link Mode Off) follows Pan Docs' description: its
status, the print command's palette and exposure, compressed image data, and a packet dropped after
100 ms. Prints that run on without a paper cut are joined, as on the real paper, and each printout
is saved as a PNG in the save folder. Printing takes a second for every eight rows, as SameBoy has
it, since the real printer's speed isn't documented.

### Link play

Link Mode picks how consoles link. Off, the default, is one console, with RetroArch's usual netplay.
There, Shared Controls lets more players play it, and the first time another player's buttons do
nothing, armGBA says so.

| You want to... | Use |
| --- | --- |
| Play alone | Link Mode Off |
| Play one game with everyone's buttons at once, on one device or over netplay | Link Mode Off, Shared Controls: Everyone |
| Take turns on one game, each player taking over with the Switch Controller Combo (Start + L2 + R2) | Link Mode Off, Shared Controls: Pass Around |
| Play together on one device, or with smooth netplay | Together |
| Link with someone who has only their own game, or whose device is too slow for two consoles | Separate |
| Trade or battle over the Wireless Adapter across the network | Separate with the Wireless Adapter |
| Play a game that links every frame online | Together |
| Fix a Game Boy game that errors in Separate | Game Boy Link Timing: Exact |
| Be a Game Boy Advance plugged into a GameCube game in Dolphin | GameCube |

**Together:** every device runs every player's console, two to four of them with the Players option.
That works for multiplayer on one device, with a controller each, or with RetroArch's rollback
netplay, where the link never waits on the network. Every device needs every game.

- Three or four consoles are shown in a grid.
- Player 2's game is picked with Player 2's Game; players 3 and 4 play player 1's.
- A player on player 1's game gets a save of their own, under the game's file name in the save
  folder's armGBA/Player 2 to Player 4 folders. It starts new, or as a copy of player 1's, as New
  Players' Saves says.
- In netplay the session runs the host's games and saves, and the host writes the players' saves as
  the game closes. A device that joined keeps its own saves as they were, and the session's in the
  save folder's armGBA/Remote folders (see Backups).

**Separate:** each device runs its own player's console, with their own game and save, and the cable
crosses RetroArch's netplay. Players can run different games, such as two versions of a game that
trade with each other. It links two players; three or four play Together. Each transfer waits for
the other end, so a slow network stalls the cable.

- Game Boy Advances stay on the same cycles, as on a real cable. Games that link every frame need
  this, and it wants a short ping.
- Game Boys run at their own pace, the one waiting sending its next byte ahead. That suits Wi-Fi and
  games that link in turns. Game Boy Link Timing set to Exact times them as Game Boy Advances are.
- The Wireless Adapter needs neither: each device runs freely.

Single-pak games send a program over the cable to consoles with no cartridge, as on the real
hardware. In Separate, the player without the game needs no game file: RetroArch's Start Core starts
armGBA with no cartridge before they join, and in the app, Netplay's Your Cartridge: Removed does
the same (`--connect ADDRESS --no-cartridge`). Only what the game sends over the cable crosses the
network; armGBA never sends a game file. In Together, where every device runs every console, each
device needs the game, and Player 2's Game: No Cartridge starts player 2's console empty.

Together and Off also need the same BIOS or boot ROM on every device. In the app, a host on
Nintendo's turns away a player who doesn't have the same file, and tells both why; switching to
armGBA's in Core > System lets everyone join. RetroArch doesn't check, so set the same BIOS or boot
ROM on every device, or use armGBA's, which every device has.

The Game Boy Advance Wireless Adapter (GBA Link Port) works with Together and Separate, for games
that use Nintendo's wireless library, including their Union Room trades. The Game Boy Four Player
Adapter works with Together, and three or four Game Boys always use it.

### Backups

- **Back Up Save** copies a player's save, as it is, to a file named for the time, such as
  `Game.20261018-153012.srm`, beside their save. Player 1's goes in the save folder's armGBA/Player
  1 folder. The game carries on.
- **Load Save** restores a backup as a player's save. With two or more players it can also give one
  player another's current save or backup, as Load Save Into says. It backs up the save it replaces
  first, then restarts that player's console. It isn't available during netplay.
- **Remote saves:** a device that joined another's netplay session keeps that session's saves in
  armGBA/Remote/Player 1 to Player 4, by the session's players, and Load Save lists them as Remote
  Player 1 to 4. Unless Load Save Into names a player, each goes to the other person: Remote Player
  1 (the host) to player 2 here when there is one, and Remote Player 2 to player 1. Hosting and
  joining in turn passes both players' saves each way.
- **Keep My Netplay Progress** (off by default) applies to a device that joined Together netplay. If
  its player's console in the session started from this device's own save (sent in with Save
  Manager, or the app's Send My Save), that console's save becomes this device's own as the session
  ends, and the old one is backed up. Otherwise this device's own save is never touched.
- **Save Manager** moves saves between devices with no game loaded. Start the core with no game
  (RetroArch's Start Core, or the app's Start Save Manager on the Netplay page), host or join, then
  use Send Save, in Saves, to send one of the saves found. In RetroArch those are the saves in its
  saves folder and ROM folder, as its `retroarch.cfg` names them, and in their `gb`, `gbc` and `gba`
  folders (on ROCKNIX, `roms/gba`, `gb` and `gbc`), with each folder's armGBA/Player 2 to 4 saves.
  In the app they are its own Saves folder and game folders, as Saves and Folders sets them. The
  other device puts the save where Receive Saves Into says, backing up the save it replaces. Auto
  takes a save to player 2's on the host, and to your own save on a device that joined. So before
  playing Together, the joining player sends their save in, and afterwards the host sends Player 2's
  back, or Keep My Netplay Progress brings it home by itself.
- **Backups Kept:** 10 per player by default. A save that matches its newest backup isn't backed up
  again.
- **Player 1's Save Folder** can keep player 1's save in armGBA/Player 1 too, beside the other
  players'. When it changes, the newer save is used.

### GameCube

With Link Mode on GameCube, this Game Boy Advance is plugged into a controller port of Dolphin set
to GBA (TCP), on the same device or another on the local network. It plays its part in the GameCube
game, as a real Game Boy Advance plugged into a GameCube does. For a program the GameCube sends, set
Cartridge to Removed; either BIOS can receive it. Dolphin Address finds Dolphin on the network by
itself, or looks only on this device with This Device. The console keeps to Dolphin's time while the
game talks to it, so the two run in step.

### Pair files

A `.gb2` file (as Batocera writes them) or a `.gba2` file names the games of a link played Together,
two to four, one per line. It opens like a game from RetroArch's Load Content:

    gb:Game One.zip
    gbc:Game Two.zip

`gb:`, `gbc:` and `gba:` mean that folder beside the pair file's folder. A line can also be a path
from the pair file's folder, or a full path. Each player keeps their game's own save, where
RetroArch keeps that game's save.

## Accuracy

Anything a game's logic or the picture can observe is exact: IRQ, DMA and timer timing,
self-modifying code, the prefetch pipeline, and visible video timing. What neither can observe is
simplified where that saves work: audio is exact in content but batched in time.

Where the hardware gives no answer, these rules apply:

- **Game Boy model (Auto):** Game Boy Color if the header's color flag is `C0`, or `80` unless that
  byte is really the end of an older game's title. Otherwise Game Boy.
- **Unlicensed Game Boy mappers:** chosen by the header logo's checksum, bank-end markers and, for a
  few dumps, the header's CRC.
- **EEPROM size:** the small one until an address past its end is used.

## Testing

- **Lockstep:** twenty games compared frame by frame with the stepping CPU on all three backends,
  and every game of the Game Boy Advance set (4590) for 1500 frames on x86-64.
- **`tests/cpufuzz`:** random ARM and Thumb programs against mGBA.
- **Hardware test ROMs:** all 28 pass (jsmolka's gba-tests, FuzzARM, NanoBoyAdvance's).
- **mGBA test suite:** every scored category passes in full except Misc edge, at 6 of 12.
- **Game Boy test ROMs:** 213 of 236 screens match a SameBoy-based build. The rest are SameSuite's
  APU tests, which time the sound channels to a fraction of a CPU cycle.
- **Game Boy games:** every game of the Game Boy and Game Boy Color sets was started and compared
  with SameBoy: 2776 of 2796 and 2983 of 3055 match. The rest are not games (BIOS files, cheat
  cartridges), betas, and unlicensed carts that differ on the other cores too.
- **Save states and runahead:** frame-exact on x86-64 and AArch64 for GBA, GBC and GB. A Game Boy
  state made as one model loads as another: the machine becomes the state's model until the next
  restart.

## Performance

How much headroom armGBA leaves on a slow handheld: frames per second on one core, unthrottled, with
the cores ROCKNIX ships alongside for reference. Full speed is about 60. Game Boy Advance games ran
9000 frames with scripted button presses, Game Boy games 3000 frames from power-on.

Anbernic RG353M (RK3566, Cortex-A55 1.8 GHz), 14 October 2026:

| Game | armGBA | mGBA |
| --- | --- | --- |
| Payback | 232 | 148 |
| Mario Kart: Super Circuit | 372 | 248 |
| Final Fantasy Tactics Advance | 353 | 255 |
| Advance Wars | 320 | 202 |
| Golden Sun | 549 | 347 |

| Game | armGBA | Gambatte | mGBA |
| --- | --- | --- | --- |
| Tetris | 1918 | 1669 | 857 |
| Pokémon Red | 2555 | 1877 | 1206 |
| Super Mario Land 2 | 2260 | 1869 | 1021 |
| The Legend of Zelda: Link's Awakening | 1932 | 1593 | 903 |
| Pokémon Crystal | 1737 | 1481 | 911 |
| The Legend of Zelda: Link's Awakening DX | 1440 | 1375 | 679 |
| Shantae | 869 | 901 | 481 |
| Wario Land 3 | 1532 | 1449 | 734 |

With Threaded Video, Payback on the RG353M runs at 276, and at 288 with One Frame Late. Every game
here runs several times faster than full speed, which leaves room for runahead, fast-forward and
slower chips.

## How it works

**Recompiler with an exact fallback.** ARM and Thumb code is compiled to AArch64, 32-bit ARM or
x86-64 and counts its own cycles. When an event (timer, video line, DMA, IRQ) falls inside a block,
a copy of the block with a check before each instruction takes over. An event the instruction can't
observe runs after it. For one it can observe, a cycle-exact stepping CPU (`src/cpu/step.c`) runs
that single instruction.

**Fewer events.** On a small core an event costs far more than its own work, because handling it
pushes the game out of the instruction cache. Sound-timer overflows that only feed a FIFO or tick a
cascaded timer are batched. Video events are scheduled only where something can see them, and lines
are drawn lazily. `DISPSTAT`, `VCOUNT`, `KEYINPUT` and `IF` are read inline while they can't change.
Provably idle loops skip to the next event. So do waiting loops that span calls, once the exact CPU
has seen one go round twice the same way.

**Self-modifying code.** A per-word map catches writes into compiled code. An instruction whose
operand keeps changing is compiled to read it from guest memory. A block that alternates between a
few shapes keeps a compiled form of each. Anything else is recompiled.

**Small caches.** Block bodies are packed together, with slow paths kept elsewhere. Sequences every
block would repeat are shared routines. Guest memory is page-aligned.

**The bus.** Every access costs what the hardware's would: wait states, sequential or not, the
cartridge prefetch buffer, and the wait for H-blank when the display has VRAM to itself.

**Video and audio.** A line renderer with NEON and AVX2 paths redraws only lines whose inputs
changed, optionally on a second thread. Audio is mixed in batches between changes.

## Diagnostics

Environment variables for working on the core, read at start:

| Variable | Effect |
| --- | --- |
| `ARMGBA_STATS=1` | Recompiler counts at exit: blocks, exits, steps, code writes |
| `ARMGBA_RENDER=reference` | Draw with mGBA's loops only; `=check` draws both ways and stops at the first difference |
| `ARMGBA_PERFMAP=1` | Name compiled blocks in `/tmp/perf-<pid>.map` for `perf` |
| `ARMGBA_DUMP=<dir>` | Write each compiled block's host code there, one file a block |
| `ARMGBA_SMCLOG=1` | Log writes into compiled code |
| `ARMGBA_STEP=<mask>` | Leave the instruction kinds in the hex mask to the exact CPU |
| `ARMGBA_TRACE=<file>` (test tools) | Log each instruction and its time; `TRACE_FROM`, `TRACE_TIME` and `TRACE_COUNT` say where it starts and how long it runs |

The test tools also need an mGBA source tree with `tests/mgba-oracle.patch` applied, and its
`libmgba.a` built with `-DCOLOR_16_BIT`:

    cmake -S . -B build -G Ninja -DMGBA_DIR=<mgba> -DMGBA_LIB=<mgba-build>/libmgba.a \
          -DMGBA_BUILD_INCLUDE=<mgba-build>/include

## Known gaps

- A few unlicensed Game Boy Color cartridges with undocumented protection chips don't boot.
- Four Game Boy Advances on one device need a fast device with four cores: each console runs on a
  core of its own, and while they link they often wait for each other.
- The frontend's netplay connects only to another armgba, not to RetroArch.
- Cheat codes mGBA can't run either (Action Replay's slowdown and disabling codes) are left out. A
  code that hooks one of the game's routines runs as the frame ends instead.

## Credits

Code taken from other projects, each under its own license (see License):

- **mGBA** (Jeffrey Pfau and contributors, MPL-2.0): the software renderer, bus and I/O behavior,
  save chips, the GPIO clock, the light sensor, gyro, tilt sensor and rumble, the Matrix and Vast
  Fame cartridge mappers, the save override list, the GBA sound channels, the cheat code decoders
  and engine, the unlicensed Game Boy mapper tables, and its MBC3 `.rtc` layout, which armGBA's
  clock files carry too. It is also the yardstick the GBA side is compared with, through
  `tests/mgba-oracle.patch`.
- **Gambatte** (sinamas) as extended by **Gambatte-Speedrun** (pokemon-speedrunning/gambatte-core,
  GPL-2.0): the whole Game Boy core in `gambatte/`, with **snes_spc** (Shay Green, LGPL) inside it.
- **SameBoy** (Lior Halphon, MIT): the Game Boy screen palettes other than Grayscale, and the Game
  Boy Color and Game Boy Advance color correction curves. A SameBoy-based build is also the
  reference the Game Boy side is tested against.
- **libretro** (MIT): `libretro/libretro.h`.
- **RetroArch** (MIT, public domain): libretro-common and the LZMA SDK as RetroArch carries them, in
  `libretro/deps/` (see its README), and rcheevos, in Armulators' `deps/`.
- **stb_truetype** (Sean Barrett, public domain or MIT): the frontend's text, in Armulators' `app/`.
- **Inter** (The Inter Project Authors) and **Chakra Petch** (The Chakra Petch Project Authors),
  OFL-1.1: the frontend's fonts, in Armulators' `app/fonts/`.

Read for how the hardware behaves, without code taken:

- **NanoBoyAdvance** (fleroviux): its hardware test ROMs, which the sprite mosaic rule was worked
  out from, and its renderer, read for when a layer switched on mid-line shows.
- **hhugboy** (taizou): how unlicensed Game Boy cartridges are detected and banked.
- **gambatte-libretro**: its `.rtc` layout, which armGBA's clock files share, its turbo buttons and
  their period, and its Grayscale palette.
- **pret/pokecrystal** and **pret/pokegold**: how the games they decompile check the clock.
- **gpSP**: fast-forward on a held button.

Test ROMs: jsmolka's gba-tests, DenSinH's FuzzARM, NanoBoyAdvance's hardware tests, mGBA's test
suite and its Game Boy test collection (Blargg's tests, Gekkio's Mooneye, LIJI's SameSuite).

## AI assistance

armGBA was written with heavy use of an AI coding assistant, Anthropic's Claude (through Claude
Code). Most of the code, comments and documentation were drafted by it at Magnominus's direction. He
chose what to build and decides what goes in. Each change was tested on his PCs and handhelds first,
and the accuracy and speed claims here come from those tests, listed above, not from the assistant's
word.

## License

Distributed under the GNU General Public License, version 2 (`LICENSE`), because the core links
Gambatte. Each part keeps its own notice:

| Part | License |
| --- | --- |
| `src/`, `tests/`, the `libretro/` glue | Mozilla Public License 2.0 (`LICENSES/MPL-2.0.txt`); files that follow mGBA's code carry its copyright notice too |
| `gambatte/` | GPL, version 2 (`gambatte/COPYING`) |
| `gambatte/libgambatte/src/mem/snes_spc/` | GNU LGPL |
| `libretro/libretro.h` | MIT |
| `libretro/deps/` | MIT and public domain, as each file states |
| `app/`, `app.cmake`, `shaders/`, `bios/`, `gbboot/` | Mozilla Public License 2.0 |
| `armulators/` | its own notices: GNU GPL, version 2 or later, with the fonts under the SIL Open Font License 1.1 and its `deps/` MIT and public domain |
| The Game Boy palettes (other than Grayscale) and color curves in `libretro/gb-gambatte.cpp` | MIT, from SameBoy (Lior Halphon) |
