# What the standalone app (Armulators' app/) is for this core: its names, Android package, look and the
# games it opens. CMake writes them into app-info.h for the C code.
set(APP_ID armgba)
set(APP_NAME armGBA)
set(APP_WORDMARK ARMGBA)
set(APP_ACCENT Blue)
set(APP_JAVA_PACKAGE org.armgba.app)
set(APP_JAVA_CLASS Armgba)
# Its version, and how it's built: the CMake option that turns it on, and its target.
set(APP_VERSION 2.0)
set(APP_BUILD_OPTION ARMGBA_SDL)
set(APP_BUILD_TARGET armgba-sdl)
# For the desktop: what it is, and the files it opens.
set(APP_COMMENT "Game Boy Advance, Game Boy, Game Boy Color and Mega Duck emulator")
set(APP_MIME application/x-gba-rom application/x-gameboy-rom application/x-gameboy-color-rom application/zip)
# The console's screen: the window is a multiple of it before a game says its own.
set(APP_WINDOW 240 160)
# The game list's files (archives too), and what's looked for inside an archive.
set(APP_EXTENSIONS gba gb gbc zip 7z gba2 gb2 bin duck md0 md1 md2 md20s md25s)
set(APP_ARCHIVED gba gb gbc bin gba2 gb2 duck md0 md1 md2 md20s md25s)
# The systems: each one's key (its folder setting is <key>_folder), name, and the folder names it's
# found by in the Games folder (its key and these, any case).
set(APP_SYSTEMS gb gbc gba megaduck)
set(APP_SYSTEM_gb "Game Boy" gameboy "game boy")
set(APP_SYSTEM_gbc "Game Boy Color" gameboycolor "game boy color")
set(APP_SYSTEM_gba "Game Boy Advance" gameboyadvance "game boy advance")
set(APP_SYSTEM_megaduck "Mega Duck" megaduck "mega duck" cougarboy "cougar boy")
# The keyboard's default keys, as SDL names them (NONE for none), in libretro's order: B, Y, Select,
# Start, Up, Down, Left, Right, A, X, L, R, L2, R2, L3, R3; then, for keys that move the sticks, the
# left stick's up, down, left and right and the right stick's. APP_BUTTON_NAMES names them in the
# menus, - for one the core doesn't use (all 24 with the stick keys), and APP_BUTTON_ORDER lists them
# by libretro's numbers; both have defaults.
# mGBA's keys; C and V are the turbo buttons, 1 to 4 L2 to R3.
set(APP_KEYS Z C BACKSPACE RETURN UP DOWN LEFT RIGHT X V A S 1 2 3 4)
# Help texts that name the console's buttons and what the app draws over its picture.
set(APP_HELP_SWAP "Off: A is the right button, as on a Game Boy. On: A and B swap, and X and Y.")
