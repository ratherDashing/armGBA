/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_CORE_H
#define ARMGBA_LIBRETRO_CORE_H

// What the libretro core's own files share, by the file that keeps it. Nothing here is seen
// outside the core.

#include "gb.h"
#include "gba.h"
#include "libretro.h"
#include "games.h"
#include "savefiles.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static inline int clampInt(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

enum {
	MAX_PLAYERS = 4,
	// Buffer sizes: a path RetroArch gives, a save's file made from one, and a backup's, which adds
	// a stamp.
	PATH_SIZE = 4096,
	SAVE_PATH_SIZE = 8192,
	BACKUP_PATH_SIZE = SAVE_PATH_SIZE + BACKUP_STAMP,
	// Player 2's Game's choices.
	SECOND_CHOICES = 125,
	// Turbo Button Period's range, in frames.
	TURBO_FEWEST = 4,
	TURBO_MOST = 120,
};

// libretro.c: the frontend's calls back, and the game and consoles running.
extern retro_environment_t environment;
extern retro_video_refresh_t videoRefresh;
// Player 1's Game Boy Advance, or NULL with a Game Boy game; each player's, when they run here.
extern struct GBA* gba;
extern struct GBA* gbas[MAX_PLAYERS];
extern int playersHere;
extern bool skipBios;
extern bool gameBoy;
extern enum LibretroGbModel gbModel;
// The original Game Boy's screen colors: 0 plain grays, 1 the Pocket's, 2 the first model's, 3 the
// Light's.
extern int gbShades;
// Game Boy Advance Panel: a Game Boy game on the Advance model shows the Color's panel.
extern bool gbColorPanel;
// Game Boy Advance consoles linked here, run in turns.
extern bool gbaTogether;
// On the Game Boy Advance model, a Game Boy game is shown as that console shows it.
extern bool gbOnAdvance;
// Whether the Game Boy cartridge has a tilt sensor (MBC7).
extern bool tiltCartridge;
// A Mega Duck game is loaded
extern bool megaDuckGame;
// An Action Replay is the game loaded, with a game on top of it or none.
extern bool actionReplayLoaded;
// The cheat cartridge loaded is Galoob's Game Genie (for the Game Boy), which has no button or switch
extern bool gameGenieLoaded;
extern bool xploderLoaded;
// The Xploder Advance or CodeBreaker for the Game Boy Advance (which have no button or switch)
extern const char* fcdLoaded;
bool replayHasButton(void);
// How many cheats RetroArch's database has for the game on top of the Game Genie
size_t genieCheatCount(void);
// Whether Import Cheats from RetroArch works with the cheat cartridge loaded
bool cheatImportable(void);
bool cheatRestorable(void);
enum { THREADED_OFF, THREADED_ON, THREADED_LATE };
extern int threadedVideo;
// The threads that draw with Threaded Video: 1 or 2.
extern int videoThreads;
// Whether RetroArch is running this frame again, for a netplay rollback or runahead, rather than
// for the first time.
bool rerunning(void);

// The games: player 1's file, player 2's (empty for player 1's) and players 3 and 4's from a pair
// file; whether the others have no cartridge; the pair file; Player 2's Game's choices.
extern char firstPath[PATH_SIZE];
extern char secondPath[PATH_SIZE];
extern char otherPaths[MAX_PLAYERS][PATH_SIZE];
extern bool othersBare;
extern bool pairFile;
extern char pairPath[PATH_SIZE];
extern struct ListedGame secondGames[SECOND_CHOICES];
extern size_t secondCount;
// The game player runs, as a path.
const char* gameOf(int player);
// Restarts player's console, as its power switch would.
void restartConsole(int player);
// Players whose save starts as a copy of player 1's, made before the first frame.
extern uint8_t copySaves;
// Whether rollback netplay has been seen this session, and whether this device joined another's.
extern bool netplaySeen;
extern bool netplayJoined;
// A Game Boy Advance's save memory size, as large as the largest until the game shows its type.
size_t saveSize(struct GBA* machine);
// A message on screen for frames frames; showMessage logs it as an error too.
void notify(const char* message, unsigned frames);
void showMessage(const char* message);

// ereader.c: e-Reader cards from the frontend's disc menu, set up as a game starts; the first frame
// calibrates a blank e-Reader.
void ereaderStart(void);
void ereaderFrame(void);
void ereaderForget(void);

// cht.c: RetroArch's cheat database. chtDatabase finds its folder (systemFolder is the frontend's
// system folder); chtCheats reads the cheats it has for the game at gamePath, those whose every code
// accepts takes, the file named as the game first, leaving out cheats already read; returns how many.
// With gameId (a Game Boy Advance game's, gbaGameId; -1 for none), of the files for the game's title
// and regions it reads those whose CodeBreaker master code names that build, when there are any;
// otherwise all of them, and *otherId (when given) is the build theirs names, else -1. *rejected
// (when given) counts the cheats there with codes accepts doesn't take (Game Genie codes aside, for
// chtGameSharkCode); a value a template leaves to fill (XX in a code's value) is read as 0, and
// *zeroed (when given) counts the cheats taken so.
struct ChtCheat {
	char description[96];
	char code[160];
	// The code as RetroArch writes it, a template's XX still there (code has 0 in its place)
	char written[160];
	// Which of the database's files it came from, in the order read
	unsigned file;
};
bool chtDatabase(const char* systemFolder, char* out, size_t outSize);
size_t chtCheats(const char* database, bool advance, const char* gamePath, int gameId, int* otherId, bool (*accepts)(const char* code),
                 struct ChtCheat* cheats, size_t most, size_t* rejected, size_t* zeroed);
// gbacheats.c: the ID the Future Console Design cartridges and CodeBreaker master codes know a Game
// Boy Advance game by, a CRC-16 of its first 64 KiB.
int gbaGameId(const uint8_t* game, size_t size);
bool chtGameGenieCode(const char* code);
bool chtGameSharkCode(const char* code);
bool chtCodeBreakerPart(const char* code);
// gbacheats.c: fcdImport adds cheats to the Xploder Advance's or CodeBreaker's database in its flash chip, under
// the game (gameSize bytes at game) its master code knows; returns how many, -3 when it has no
// master code for the game, or -1 when the database can't be read. fcdCipherWorks checks the cipher.
int fcdImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
              size_t* already, size_t* left, size_t* unfit);
bool fcdCipherWorks(void);
// arGbaImport adds them to the Action Replay v3's or GBX's list in its flash chip, likewise (and -4
// for the Action Replay MAX's, which it doesn't know)
int arGbaImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
                size_t* already, size_t* left, size_t* unfit);
bool arGbaCipherWorks(void);
// maxImport adds them to the Action Replay MAX's code list file for the game, likewise
int maxImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
              size_t* already, size_t* left, size_t* unfit);
// Factory Reset (keepMasters: each game keeps its master code, which runs and finds it) and Full Wipe
// (the list emptied) of the Xploder Advance's or CodeBreaker's database, the Action Replay v3's or
// GBX's list (-4 for the MAX's) and the MAX's files (its code lists, and the other files, such as DS
// game saves, gone either way). 0, or -1 when it can't be read.
int fcdClear(uint8_t* flash, size_t size, bool keepMasters);
int arGbaClear(uint8_t* flash, size_t size, bool keepMasters);
int maxClear(uint8_t* flash, size_t size, bool keepMasters);

// printout.c: the Game Boy Printer's paper, saved as a picture in the save folder once it is cut, or
// as the game closes.
void printoutPage(const uint8_t* pixels, int rows, bool cut, uint32_t number);
void printoutFinish(void);

// saves.c: each player's save memory and files, the saves the core keeps itself (the other
// players', and player 1's with a pair file or Player 1's Save Folder), and the Saves options.
void* saveMemory(int player, size_t* size);
// Reads a file into memory, as much as fits; returns whether there was a file.
bool readFileInto(const char* path, void* data, size_t size);
// Where RetroArch keeps player 1's save and clock, and whether the core keeps player 1's instead.
void retroArchFiles(char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]);
bool retroArchSaves(void);
// Reading saves as games start: player 1's, the newer of RetroArch's and the core's; a player's of
// a game of their own; the other players'. readNewerPlayerOne reads player 1's again each frame
// RetroArch wrote it.
void readPlayerOneSave(void);
void readNewerPlayerOne(void);
void saveFilesFor(const char* game, char file[SAVE_PATH_SIZE], char rtcFile[SAVE_PATH_SIZE]);
// The times of games' saves where saveFilesFor finds them, each folder listed once (savefiles.h).
struct SaveTimes* gameSaveTimes(void);
bool readSave(int player, const char* game);
void readOtherSaves(void);
void copyFirstSave(void);
// Writes the kept saves that changed (in netplay only as the game closes, on the host); forgets
// them; writeKeptSaves checks every few seconds.
void writeCoreSaves(bool closing);
void forgetSaves(void);
// Forgets one player's kept save, which isn't written after.
void forgetSave(int player);
void forgetOtherSaves(void);
void writeKeptSaves(void);
// The Saves options: found as the frontend sets up, their choices filled and offered, and their
// actions, done once when chosen (or only reset when act is false, as a game starts).
void findSaveOptions(void);
void fillSaveChoices(void);
void offerSaveChoices(void);
void readSaveActions(bool act);
void saveOptionsChanged(void);
// Netplay's saves: a joined session's kept for Load Save, and Save Manager's received.
void keepSession(void);
// Keep My Netplay Progress: on a device that joined, this device's player, and whether its console
// holds this device's own save, looked at as the host's states load.
int joinedPlayer(void);
void checkSessionSave(void);
void saveReceived(const char* game, const uint8_t* save, size_t size, const uint8_t* rtc, size_t rtcSize);

// options.c: the options given to the frontend (NULL to an older frontend, which has plain
// variables), and their values. optionChoice gives the number of the option's value among choices,
// or fallback if it isn't one. The Wanted functions read Link Mode, Number of Players and GBA Link
// Port. showOptions shows the options that do something for the game and Link Mode, and returns
// whether that changed; setOptions gives the options again after a list of choices changed.
extern struct retro_core_options_v2* coreOptions;
struct Choice {
	const char* value;
	int number;
};
bool optionsGive(retro_environment_t callback);
struct retro_core_option_v2_definition* optionDefinition(const char* key);
const char* option(const char* key);
bool optionIs(const char* key, const char* wanted);
// Writes a whole file, the old one kept whole until the new one is (Safe Saves); false if it couldn't.
bool writeWholeFile(const char* path, const void* data, size_t size);
// Reads a whole file, to be freed; NULL if there is none, or it is empty.
void* readWholeFile(const char* path, size_t* size);
int optionChoice(const char* key, const struct Choice* choices, size_t count, int fallback);
bool togetherWanted(void);
bool separateWanted(void);
bool gamecubeWanted(void);
int playersWanted(void);
bool adapterWanted(int count);
bool showOptions(void);
void setOptions(void);

// clock.c: the Clock options, read as the game starts (act false) and as they change, when Reset Clock
// acts; the Game Boy Advance cartridge clock's time, given before each frame; a console's .rtc file in
// Game Time; the clocks forgotten as the game closes.
void readClockOptions(bool act);
void clockFrame(void);
void* clockMemory(int player, size_t* size);
void clockForget(void);

// lcd.c: LCD Ghosting (0 off, then 1 to 4 quarters) and Interlaced Rows; LCD Scale in halves (2 when
// off), whether it draws Pixel Gaps, and the pixel shadow's way (0 none, then up and on clockwise);
// the threads they share rows among (0 chooses). showPicture sends the picture on through them all,
// sendPicture through LCD Scale's alone.
extern int ghosting;
extern bool interleaved;
extern int gridHalves;
extern int gridWay;
extern bool gridGaps;
// LCD Scale's Fit Screen, and the screen size it fits.
extern bool gridFit;
extern unsigned gridFitWidth, gridFitHeight;
extern int lcdThreads;
// Frame Skip: whether this frame is left out of the picture, and how many before it were.
extern bool frameSkipped;
extern int framesSkipped;
void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch);
void sendPicture(const void* picture, unsigned width, unsigned height, size_t pitch);
// Whether the screen is the first Game Boy's or the Pocket's, whose pixels cast a shadow.
bool lcdCastsShadow(void);
// Whether the screen is the Color's or the Advance's, whose pixels are colored stripes.
bool lcdHasSubpixels(void);

#endif
