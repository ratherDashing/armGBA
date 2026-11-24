/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LINK_H
#define ARMGBA_LIBRETRO_LINK_H

// The link cable between two players' consoles in a netplay session, carried over RetroArch's
// netpacket interface. Each player runs their own game and save; the host is player 1. A transfer waits for
// the other end's data, so a link is smooth on a local network and slow over a long one.

#include <stdbool.h>
#include <stdint.h>

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GBA;
struct RfuAir;

// Offers the frontend the netpacket interface.
void linkSetEnvironment(retro_environment_t);
// This device's player in the standalone frontend's netplay, which RetroArch's doesn't say; -1 when
// not in it.
void libretroSetNetplayPlayer(int player);
// The firmware from the system folder the running consoles started with, Nintendo's BIOS or boot
// ROMs, as a CRC (0 when they run armGBA's or none), and that option's name. Netplay devices running
// every console must match.
uint32_t libretroFirmwareCrc(const char** option);
// A joining player's own save in the standalone frontend's Together netplay, put in that player's
// console on the host, the save it replaces backed up, and the console restarted; returns why not, or
// NULL.
const char* libretroTakeSave(int player, const void* save, size_t size, const void* rtc, size_t rtcSize);
// A session this device joined ended, in the standalone frontend or RetroArch: with Keep My Netplay
// Progress, the player's progress becomes this device's own save.
void libretroSessionEnded(void);
// A session this device joined ended and the game plays on, controller 1 staying on this device's player.
void libretroPlayOn(void);
// The loaded console: a Game Boy Advance, a Game Boy (gba NULL), or none.
void linkSetGba(struct GBA*);
void linkSetGameBoy(bool loaded);
// The Wireless Adapter's air to the other player, and whether the adapter is on the link port
// instead of the cable.
const struct RfuAir* linkAir(void);
void linkSetWireless(bool);
// Whether a netplay session is on, during which time runs at the same speed for every player.
bool linkSession(void);
// Whether this device hosts the session.
bool linkHosting(void);
// Whether the core started with no game: Save Manager, or waiting for a single-pak game.
bool noGameLoaded(void);
// The standalone frontend's folders for Save Manager, from its own settings, in place of RetroArch's
// retroarch.cfg: save folders (searched with their gb, gbc, gba and armGBA folders), game folders
// (with their gb, gbc and gba folders), and whether the game folders come first, as with saves kept
// beside the games.
void libretroSaveManagerFolders(const char* const* saves, int saveCount, const char* const* games, int gameCount, bool gamesFirst);
// Called before each Game Boy Advance frame: keeps the cable checked during it.
void linkGbaFrame(struct GBA*);
// Whether the frame stopped early, with exact timing, to wait for the console leading; it goes on
// next time.
bool linkGbaHeld(void);
// Whether exact timing is on: for Game Boy Advances on the cable, and Game Boys when chosen.
bool linkExactOn(void);
// The Game Boy Link Timing option: whether a Game Boy's cable is exact.
void linkSetGbExact(bool on);
// Sends this device's save of a game (named without its extension), and clock, to the other player,
// a piece a frame. Returns false with no one linked.
bool linkSendSave(const char* game, const void* save, size_t saveSize, const void* rtc, size_t rtcSize);
// Called when the other device's save has arrived.
void linkSetSaveHook(void (*arrived)(const char* game, const uint8_t* save, size_t saveSize, const uint8_t* rtc, size_t rtcSize));
// Offers RetroArch the netplay that carries the cable (Separate), or leaves it its usual netplay.
void linkSetNetplay(bool on);
// Game Boy: whether another Game Boy is on the cable, a transfer this one clocked (returns the byte
// received), and a check for transfers the other one clocked.
bool linkGbConnected(void);
int linkGbTransfer(int out);
void linkGbPoll(void);
// Game Boy with exact timing.
// A transfer it clocks is under way: samples left until it ends, and its length.
void linkGbClocking(int out, long left, long length);
// The samples it may run before its next stop.
long linkGbAllowance(void);
void linkGbRan(unsigned samples);
// At its stop: takes the transfer due there, or waits for the leading console. False if it must
// still wait, which ends the frame early.
bool linkGbArrived(void);

// The standalone app's copy of RetroArch's cheat database, for Import Cheats from RetroArch when
// RetroArch's own isn't found (app/cheatdb.c). fetch puts the folder that has the cheats for the game
// at gamePath in out and returns CHEAT_FETCH_READY, or with start downloads them if it has none,
// returning CHEAT_FETCH_BUSY until they're in; CHEAT_FETCH_NONE when they can't be had.
enum { CHEAT_FETCH_NONE, CHEAT_FETCH_READY, CHEAT_FETCH_BUSY };
void libretroSetCheatFetch(int (*fetch)(const char* gamePath, bool advance, bool start, char* out, size_t outSize));
// While it's busy: the KiB downloaded and in all (0 until known); false once it's unpacking.
void libretroSetCheatProgress(bool (*progress)(int* doneKib, int* allKib));
// The cheat cartridge's game on top, for Import Cheats from RetroArch: false when there's none to
// import for (no cartridge that takes them, or no game on top, or netplay). retroArchs is whether
// RetroArch's own cheat database is found, which the import reads then.
bool libretroCheatImportFor(const char** gamePath, bool* advance, bool* retroArchs);
// The game RetroArch's cheat database has cheats for here: the game on top of a cheat cartridge, else
// the game itself (player 1's); false with none, or a Mega Duck's. advance: whether it's the Advance's.
bool libretroCheatGame(const char** gamePath, bool* advance);
// RetroArch's own cheat database's folder, when it's found.
bool libretroRetroArchCheats(char* out, size_t outSize);
// What Import Cheats from RetroArch would do with a cheat: add it, find it there already, leave it out
// (its codes of a kind the cartridge doesn't run, or with a value to pick in an address), find no room
// for it, or skip it (only a master code, which the cartridge keeps its own of).
enum { CART_CHEAT_IMPORT, CART_CHEAT_THERE, CART_CHEAT_CANT, CART_CHEAT_NO_ROOM, CART_CHEAT_SKIPPED };
// The cheats RetroArch's cheat database at folder has for the cheat cartridge's game on top, in forms
// the import reads: their names, their codes as RetroArch writes them and what the import would do with
// each (states, a CART_CHEAT_). Returns how many, up to most. *otherId is the build RetroArch's codes
// are for when it isn't the game on top's (*gameId), else -1; *unread how many more it has in forms the
// import doesn't read.
size_t libretroCartridgeCheatNames(const char* folder, char (*names)[96], char (*codes)[160], unsigned char* states, size_t most, int* otherId,
                                   int* gameId, size_t* unread);
// How the loaded cheat cartridge's codes are written, where a value is, or NULL without one that imports.
const char* libretroCartridgeCodeLayout(void);
// The kind of codes the loaded cheat cartridge's list holds: 0 without one that imports, 1 GameShark
// codes (the Game Boy's), 2 CodeBreaker codes (the Future Console Design cartridges'), 3 encrypted
// Action Replay or GameShark codes (the other Game Boy Advance cartridges').
int libretroCartridgeCodeKind(void);
// Whether the .cht file named chtName in RetroArch's cheat database is for the game at gamePath: the
// same title, and a region of its (cht.c).
bool chtFileFits(const char* chtName, const char* gamePath);

#ifdef __cplusplus
}
#endif

#endif
