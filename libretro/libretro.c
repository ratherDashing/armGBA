/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core. Game Boy Advance games run on armGBA; Game Boy and Game Boy Color games run on
// the Gambatte core in gambatte/.

#include "libretro.h"
#include "core.h"

#include "cheats.h"
#include "gba.h"
#include "gb.h"
#include "lcd.h"
#include "dolphin.h"
#include "link.h"
#include "games.h"
#include "savefiles.h"
#include "patch.h"
#include "workers.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

enum {
	AUDIO_RATE = 32768,
};

#define FRAME_RATE (16777216.0 / 280896.0)

retro_environment_t environment;
retro_video_refresh_t videoRefresh;
static retro_audio_sample_batch_t audioBatch;
static retro_input_poll_t inputPoll;
static retro_input_state_t inputState;
static retro_log_printf_t logPrintf;

struct GBA* gba;
bool skipBios;
// The CPU Clock option, as quarters above the console's clock (cpu.overclock). With CPU Clock
// During Boot at 100%, the BIOS intro or boot ROM runs on the console's clock until the game starts.
static int8_t cpuOverclock;
static bool stockDuringBoot;
static bool booting;
enum { TILT_LEFT_STICK, TILT_RIGHT_STICK, TILT_DPAD, TILT_DEVICE };
static int tiltControl;
// Device Motion: the frontend's sensors, and which of them are on.
static struct retro_sensor_interface sensors;
static bool accelerometerOn, gyroscopeOn;
// Device Motion Sensor Rotation: quarter turns of the screen from the sensor's own axes.
static int motionRotation;
static void useSensors(bool tilt, bool gyro);
static bool leftStickDpad;
enum { RESTART_L_R_START, RESTART_L_R_A_B, RESTART_STICKS, RESTART_SELECT_START_A_B, RESTART_OFF };
static int restartButtons;
// Turbo timing: each press-and-release cycle lasts turboPeriod frames, and the button is held for
// the first turboPulse of them.
static unsigned turboPeriod = TURBO_FEWEST;
static unsigned turboPulse = 2;
// Boktai's sunlight, 0 to 10.
static int solarLevel = 5;
// What the buttons have done that a state holds, so a rollback replays them alike: each turbo
// button's place in its period, and whether the Solar Sensor's stick and the tilt flick were held.
static struct {
	uint8_t turboA[MAX_PLAYERS], turboB[MAX_PLAYERS], turboL, turboR;
	uint8_t solarHeld, flickHeld;
} pressed;
// The same for the Action Replay's button and L3, in a part of the state of their own, after the rest
// (a state from before has none, and they start up): Hold Action Replay Button, whether its stick
// click was held, the frames left of Open Action Replay Menu's press; whether L3 was held and for how
// many frames, and the frames left of its soft reset.
static struct {
	uint8_t replayHold, replayClicked, replayPressing;
	uint8_t l3Held, l3HeldFor, softResetFrames;
} controls;
// Shared Controls with one console: everyone's buttons, or one player's at a time, passed around.
enum { SHARED_OFF, SHARED_EVERYONE, SHARED_PASS };
static int sharedControls;
// Link Mode is off, and whether this game has said that other players' buttons do nothing then.
static bool oneConsole, sharedHinted;
// Who has the console with Pass Around, whose controllers hold the Switch Controller Combo, and
// whether the one who just took over still holds its buttons, which the game doesn't see; kept in
// a state, after the rest, so a rollback replays it alike.
static struct {
	uint8_t holder, comboHeld, taking;
} passed;
static retro_set_rumble_state_t setRumble;
static bool rumbling;
int threadedVideo;
static bool threadedAudio;
int videoThreads = 1;
// Color correction: each RGB565 color, green's low bit ignored, to its corrected color.
static bool colorCorrection;
static uint16_t* correctedColors;
static uint16_t correctedFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
// Whether RetroArch is to hear of a new picture size, as LCD Grid's scale changed.
static bool gridAnnounce;
static int32_t lowPass;
bool gameBoy;
enum LibretroGbModel gbModel;
static bool gbColorCorrection;
// The original Game Boy's screen colors: 0 plain grays, 1 the Pocket's, 2 the first model's, 3 the
// Light's.
int gbShades;
static int gbContrast;
// On the Game Boy Advance model, a Game Boy game is shown as that console shows it: centered on its
// screen, or stretched across it. When cropped, the black border is left out.
bool gbOnAdvance;
// Game Boy on GBA: Screen: a Game Boy game on the Advance model shows the Color's panel.
bool gbColorPanel;
static bool gbStretched;
static bool gbCropped;
static uint32_t gbAdvanceFrame[SCREEN_WIDTH * SCREEN_HEIGHT];
// Game Boys linked here, a controller each, by the cable (two) or the Four Player Adapter.
static bool gbTogether;
static bool gbAdapter;
static uint32_t gbTogetherFrame[LIBRETRO_GB_WIDTH * 2 * LIBRETRO_GB_HEIGHT * 2];
// The console's buttons, as the Game Boy Advance's KEYINPUT register has them.
enum {
	KEY_A = 0x1,
	KEY_B = 0x2,
	KEY_SELECT = 0x4,
	KEY_START = 0x8,
	KEY_RIGHT = 0x10,
	KEY_LEFT = 0x20,
	KEY_UP = 0x40,
	KEY_DOWN = 0x80,
	KEY_R = 0x100,
	KEY_L = 0x200,
};
int playersHere = 1;
// Players whose save starts as a copy of player 1's, made before the first frame.
uint8_t copySaves;

// The consoles of a Game Boy Advance link can also run here: up to four on one cable, each on its
// own thread.
//
// They run in rounds that all end at the same moment. Between rounds, with every console stopped,
// the link data is passed in a fixed order, so thread timing never changes the result.
//
// A round ends where a transfer ends, so the consoles swap data on its last cycle, as the wires
// would. No round is longer than the shortest transfer a console could start, so none is missed.
//
// Each console starts part of a frame after the one before, because real consoles are never in
// step.
enum {
	GBA_FRAME_CYCLES = 280896,
	ROUND_LONGEST = GBA_FRAME_CYCLES,
};
struct GBA* gbas[MAX_PLAYERS];
bool gbaTogether;
static int32_t gbaAhead[MAX_PLAYERS];
static struct Event sliceEvents[MAX_PLAYERS];
static bool sliceOver[MAX_PLAYERS];
static int32_t roundDone[MAX_PLAYERS];
// The cable: each console's transfer, and the others' data for it, handed over between rounds. A
// transfer can end in the frame after it starts, so the transfers are part of a state.
enum { MULTIPLAYER = 2 };
static struct SioLink cables[MAX_PLAYERS];
static struct Transfer {
	int32_t mode;
	uint32_t out;
	bool waiting;
	bool answered;
	uint32_t in[3];
	// The other consoles' data, and when to take it: as the transfer starts in normal mode, since
	// both ends shift out from the first bit, or as it ends in multiplayer mode, which Kirby & The
	// Amazing Mirror needs with four players. pending has a bit for each console whose data is
	// still to be taken.
	uint32_t data[MAX_PLAYERS];
	int32_t takeAt[MAX_PLAYERS];
	uint8_t pending;
} transfers[MAX_PLAYERS];
// Each console's writes to the data it sends in a round are kept, by when, with its data as the
// round began, so its data at a moment inside the round can be told after it.
enum { DATA_WRITES = 8 };
static struct {
	uint32_t before;
	int count;
	int32_t at[DATA_WRITES];
	uint32_t value[DATA_WRITES];
} dataWrites[MAX_PLAYERS];
static uint32_t roundStartTime[MAX_PLAYERS];
// The Wireless Adapters of the consoles here: their messages are carried between rounds.
enum { AIR_QUEUE = 16, AIR_BYTES = 100 };
static struct RfuAir airs[MAX_PLAYERS];
static struct {
	int8_t to;
	uint8_t size;
	uint8_t data[AIR_BYTES];
} airQueue[MAX_PLAYERS][AIR_QUEUE];
static int airQueued[MAX_PLAYERS];
static uint16_t gbaShown[MAX_PLAYERS][SCREEN_WIDTH * SCREEN_HEIGHT];
static uint16_t gbaTogetherFrame[SCREEN_WIDTH * 2 * SCREEN_HEIGHT * 2];
static uint16_t gbaCorrected[MAX_PLAYERS][SCREEN_WIDTH * SCREEN_HEIGHT];

// How the screens are shown: side by side, stacked, a grid, or one player's alone (SCREENS_ONE +
// player).
enum { SCREENS_SIDE, SCREENS_STACKED, SCREENS_ONE, SCREENS_COUNT = SCREENS_ONE + MAX_PLAYERS };
static int screens;
static int screensOption = -1;
// The L2 Button option: fast-forward, or the previous screen layout.
static bool l2FastForward = true;
// L2 and R2 as Turbo L and R (Game Boy Advance): then they only press them, with no fast-forward or
// screen layouts.
static bool turboOnTriggers;
// L2 and R2's Fast-Forward Toggle: a press turns fast-forward on until the next, the same latch as
// L3's toggle (fastForwardLatched), so either button turns it off.
static bool triggersToggle;
#define TRIGGERS_TURBO (turboOnTriggers && !gameBoy)
// L3 Button: what clicking L3 does where nothing else has it; fast-forward latched by its toggle;
// and the frames left of its soft reset's A + B + Select + Start, pressed once it's held long enough.
enum { L3_FAST_FORWARD, L3_SOFT_RESET, L3_OFF };
enum { SOFT_RESET_HOLD = 30, SOFT_RESET_PRESS = 10 };
static int l3Button = L3_FAST_FORWARD;
static bool fastForwardLatched;
// Screen Layout Control: in netplay, whose L2 and R2 change this device's screens, -1 for Auto.
// Show Player Numbers: each player's number on their screen.
static int r2Follows = -1;
static bool playerNumbers;
// This device's player in the standalone frontend's netplay, -1 when not in it.
static int netplayPlayer = -1;

// RetroArch after 1.22.2 says which netplay client this device is: 0 the host, then the devices that
// joined, each playing the player of its number.
#ifndef RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX
#define RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX (82 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif
// Its answer, asked each frame as a device that joins gets its number only once connected; -1 none.
static int netplayClient = -1;
// A state from another device's session loaded: Keep My Netplay Progress looks at it next frame.
static bool sessionSaveDue;
// RetroArch said which client this device is in a session it joined, and the session's end was seen.
static bool clientTold, sessionOver;
// Whether this RetroArch doesn't know the call that says which netplay client this device is (1.22.2
// and earlier log each try).
static bool clientIndexUnknown;
static bool askClientIndex(unsigned* client);
static int16_t mixedSamples[2 * 4096];

// Player 2's game: the choices offered, and the game running (empty for player 1's).
static struct retro_core_option_v2_definition* secondOption;
// Mega Duck Laptop Cartridge, whose choices are the Mega Duck cartridges beside the laptop program
static struct retro_core_option_v2_definition* slotOption;
static struct retro_core_option_v2_definition* replayOption;
static struct retro_core_option_v2_definition* genieOptions[3];
static struct ListedGame slotGames[SECOND_CHOICES];
static size_t slotCount;
static char slotFirst[PATH_SIZE];
struct ListedGame secondGames[SECOND_CHOICES];
size_t secondCount;
char firstPath[PATH_SIZE];
char secondPath[PATH_SIZE];
static bool secondFromFrontend;
bool othersBare;
bool pairFile;
char pairPath[PATH_SIZE];
// Players 3 and 4's games, empty for player 1's: from a pair file.
char otherPaths[MAX_PLAYERS][PATH_SIZE];
static int pairCount;
bool netplaySeen;
// The Switch Controller Combo buttons move controller 1 to the next console here; controllerFocus
// is the console it plays, the other controllers moving along with it.
enum { SWITCH_OFF, SWITCH_L_R_A_B, SWITCH_L3_R3, SWITCH_START_L2_R2, SWITCH_START_L3, SWITCH_START_R3 };
static int switchController = SWITCH_START_L2_R2;
static int controllerFocus;
// A session this device joined ended and the game plays on, controller 1 on this device's
// player's console, as without netplay.
static bool playingOn;
static bool switchStick(unsigned button);
static bool contrastClick(void);
static bool contrastStick(void);
static void readReplayPress(bool act);
static bool switchStart(int port);
static bool l3Free(void);
static void stopFastForward(void);
// The stick click that's the Action Replay's button, or REPLAY_BUTTON_OFF; and how many more frames
// Open Action Replay Menu holds it.
enum { REPLAY_BUTTON_OFF = ~0u, REPLAY_PRESS_FRAMES = 6 };
static unsigned replayButton = RETRO_DEVICE_ID_JOYPAD_R3;
// Hold Action Replay Button: the Game Boy Advance one's button held down; and whether its stick click
// toggles that (Action Replay Button Press) rather than holding the button only while it's held.
// The Hold option as last read, so only a change made in the menu holds or lets go of the button.
static bool replayHoldOption;
static bool replayToggles = true;
// How to use the Action Replay just loaded, shown as it starts: which one it is, or none to show.
enum ReplayHint { HINT_NONE, HINT_NO_GAME, HINT_GAME_BOY, HINT_GAME_BOY_FIRST, HINT_GAME_GENIE, HINT_XPLODER, HINT_ADVANCE, HINT_FCD };
static enum ReplayHint replayHint;
// In rollback netplay every device runs the host's session. Each device tags its session, and the
// tag goes in every state. A device that loads a state with another device's tag has joined that
// session and takes its tag, so the states on every device stay the same.
static uint32_t sessionTag;
bool netplayJoined;



// Whether the Game Boy cartridge has a tilt sensor (MBC7).
bool tiltCartridge;
bool megaDuckGame;
bool actionReplayLoaded;
bool gameGenieLoaded;
bool xploderLoaded;
const char* fcdLoaded;

// Whether the cheat cartridge loaded has a button, which a stick click can be: the Action Replays and
// the Xploder GB.
bool replayHasButton(void) {
	return actionReplayLoaded && !gameGenieLoaded && !fcdLoaded;
}
// Whether the GameCube link was last shown as connected.
static bool gamecubeShown;

static void airSend(const struct RfuAir* air, int to, const uint8_t* message, size_t size);
static void cableBegin(void* context, int mode, uint32_t out, int32_t cycles);
static bool cableFinish(void* context, uint32_t in[3]);
static void cableWrote(void* context);

// Cheat codes from the frontend, by index, and the Game Boy Advance operations they decode to. All
// codes are decoded again, in order, whenever one changes, because a master code changes how the
// codes after it decode.
static char** cheatCodes;
static size_t cheatCodeCount;
static struct CheatSet cheats;
// Whether the memory map given to the frontend is a Game Boy Color's, which has more work RAM.
static bool gbMapColor;

// Two games loaded together, one per console of a link, each with its own save (and clock) files.
enum {
	SUBSYSTEM_LINK = 1,
	SUBSYSTEM_ADVANCE_LINK = 2,
	SUBSYSTEM_MEGADUCK_LAPTOP = 3,
	SUBSYSTEM_ACTION_REPLAY = 4,
	MEMORY_SECOND_SAVE = 0x100,
	MEMORY_SECOND_RTC = 0x101,
};

// Helpers.

// A Game Boy Advance's save memory: until the game shows its save type, as large as the largest, as
// in mGBA's core, so a save file from either core loads.
size_t saveSize(struct GBA* machine) {
	return machine->savedata.type == SAVEDATA_AUTODETECT ? SIZE_FLASH1M : savedataSize(machine);
}

// Whether the consoles of a link run here: Together, a pair file, or two games from the frontend.
static bool togetherRunning(void) {
	return gbTogether || gbaTogether;
}

static void logMessage(enum retro_log_level level, const char* message) {
	if (logPrintf) {
		logPrintf(level, "%s\n", message);
	} else {
		fprintf(stderr, "%s\n", message);
	}
}

void notify(const char* message, unsigned frames) {
	struct retro_message shown = { message, frames };
	environment(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
}

void showMessage(const char* message) {
	notify(message, 180);
	logMessage(RETRO_LOG_ERROR, message);
}

static void updateRumble(bool on) {
	if (on != rumbling && setRumble) {
		setRumble(0, RETRO_RUMBLE_STRONG, on ? 0xFFFF : 0);
		setRumble(0, RETRO_RUMBLE_WEAK, on ? 0xFFFF : 0);
	}
	rumbling = on;
}

// The frontend's calls to set the core up, and what the core tells it.

void retro_set_environment(retro_environment_t callback) {
	environment = callback;
	static const struct retro_subsystem_memory_info firstMemory[] = { { "srm", RETRO_MEMORY_SAVE_RAM }, { "rtc", RETRO_MEMORY_RTC } };
	static const struct retro_subsystem_memory_info secondMemory[] = { { "srm", MEMORY_SECOND_SAVE }, { "rtc", MEMORY_SECOND_RTC } };
	static const struct retro_subsystem_rom_info roms[] = {
		{ "Player 1's Game", "gb|gbc", false, false, true, firstMemory, 2 },
		{ "Player 2's Game", "gb|gbc", false, false, true, secondMemory, 2 },
	};
	static const struct retro_subsystem_rom_info advanceRoms[] = {
		{ "Player 1's Game", "gba|bin", false, false, true, firstMemory, 1 },
		{ "Player 2's Game", "gba|bin", false, false, true, secondMemory, 1 },
	};
	static const struct retro_subsystem_rom_info laptopRoms[] = {
		{ "Laptop System ROM", "md0|bin", false, false, true, firstMemory, 1 },
		{ "Cartridge in the Slot", "bin|duck|md1|md2|md20s|md25s", false, false, true, NULL, 0 },
	};
	static const struct retro_subsystem_rom_info replayRoms[] = {
		{ "Cheat Cartridge", "gba|bin|gb|gbc", false, false, true, NULL, 0 },
		{ "Game on Top", "gba|bin|gb|gbc", false, false, true, firstMemory, 1 },
	};
	static const struct retro_subsystem_info subsystems[] = {
		{ "Two Game Boys (Link Cable)", "gblink", roms, 2, SUBSYSTEM_LINK },
		{ "Two Game Boy Advances (Link Cable)", "gbalink", advanceRoms, 2, SUBSYSTEM_ADVANCE_LINK },
		{ "Mega Duck Laptop (Cartridge in the Slot)", "megaducklaptop", laptopRoms, 2, SUBSYSTEM_MEGADUCK_LAPTOP },
		{ "Cheat Cartridge (Game on Top)", "actionreplay", replayRoms, 2, SUBSYSTEM_ACTION_REPLAY },
		{ NULL, NULL, NULL, 0, 0 },
	};
	callback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*) subsystems);
	bool noGameStarts = true;
	callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGameStarts);
	struct retro_log_callback logging;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
		logPrintf = logging.log;
	}
	// The options whose choices the core fills in, with a frontend that takes them.
	if (optionsGive(callback)) {
		secondOption = optionDefinition("armgba_player2_game");
		slotOption = optionDefinition("armgba_megaduck_slot");
		replayOption = optionDefinition("armgba_action_replay_game");
		genieOptions[0] = optionDefinition("armgba_game_genie_code_1");
		genieOptions[1] = optionDefinition("armgba_game_genie_code_2");
		genieOptions[2] = optionDefinition("armgba_game_genie_code_3");
		findSaveOptions();
	}
	// Low Memory: the frontend gives the core the game's path, and the core maps the file.
	static const struct retro_system_content_info_override mapped[] = { { "gba|bin", true, false }, { NULL, false, false } };
	if (optionIs("armgba_low_memory", "enabled")) {
		callback(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*) mapped);
	}
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
	videoRefresh = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
	audioBatch = callback;
}

void retro_set_input_poll(retro_input_poll_t callback) {
	inputPoll = callback;
}

void retro_set_input_state(retro_input_state_t callback) {
	inputState = callback;
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armGBA";
	info->library_version = "1.7";
	info->valid_extensions = "gba|bin|gb|gbc|gb2|gba2|duck|md0|md1|md2|md20s|md25s";
	info->need_fullpath = false;
	info->block_extract = false;
}

// The largest picture told to the frontend since the game started. It never shrinks: RetroArch makes
// its video again for a new largest size, and its threaded video then copies the last picture, still
// the larger one, into a buffer only as large as the new size.
static unsigned largestWidth, largestHeight;

static void keepLargest(struct retro_game_geometry* geometry) {
	largestWidth = geometry->max_width > largestWidth ? geometry->max_width : largestWidth;
	largestHeight = geometry->max_height > largestHeight ? geometry->max_height : largestHeight;
	geometry->max_width = largestWidth;
	geometry->max_height = largestHeight;
}

// The picture size: 240x160, or 160x144 for a Game Boy game, or the linked consoles' screens. On the Game
// Boy Advance model, a Game Boy game is 240x160 with the border shown, or 160x144 cropped (240x144
// when stretched).
static void geometry(struct retro_game_geometry* geometry) {
	geometry->base_width = SCREEN_WIDTH;
	geometry->base_height = SCREEN_HEIGHT;
	// Room for two by two consoles' screens, which a Restart can bring in.
	geometry->max_width = SCREEN_WIDTH * 2;
	geometry->max_height = SCREEN_HEIGHT * 2;
	geometry->aspect_ratio = 3.0f / 2.0f;
	if (gameBoy && !gbOnAdvance) {
		geometry->base_width = LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = 10.0f / 9.0f;
	}
	if (gameBoy && gbOnAdvance && gbCropped) {
		geometry->base_width = gbStretched ? SCREEN_WIDTH : LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
		geometry->aspect_ratio = gbStretched ? 5.0f / 3.0f : 10.0f / 9.0f;
	}
	if (gameBoy && gbTogether) {
		geometry->base_width = LIBRETRO_GB_WIDTH;
		geometry->base_height = LIBRETRO_GB_HEIGHT;
	}
	if (gbaTogether) {
		geometry->base_width = SCREEN_WIDTH;
		geometry->base_height = SCREEN_HEIGHT;
	}
	if (togetherRunning()) {
		int const shown = screens;
		bool const grid = playersHere > 2 && shown < SCREENS_ONE;
		geometry->base_width *= shown == SCREENS_SIDE || grid ? 2 : 1;
		geometry->base_height *= shown == SCREENS_STACKED || grid ? 2 : 1;
		geometry->aspect_ratio = (float) geometry->base_width / geometry->base_height;
	}
	// Fit Screen's picture, as large as fits the screen, and never larger than it unless the picture
	// is.
	if (gridFit) {
		unsigned w, h;
		libretroLcdFitSize(geometry->base_width, geometry->base_height, &w, &h);
		libretroLcdShownScale = (float) w / geometry->base_width;
		geometry->base_width = w;
		geometry->base_height = h;
		geometry->max_width = geometry->max_width > gridFitWidth ? geometry->max_width : gridFitWidth;
		geometry->max_height = geometry->max_height > gridFitHeight ? geometry->max_height : gridFitHeight;
		keepLargest(geometry);
		return;
	}
	// The LCD Grid's larger picture.
	libretroLcdShownScale = gridHalves / 2.0f;
	geometry->base_width = geometry->base_width * (unsigned) gridHalves / 2;
	geometry->base_height = geometry->base_height * (unsigned) gridHalves / 2;
	geometry->max_width = geometry->max_width * (unsigned) gridHalves / 2;
	geometry->max_height = geometry->max_height * (unsigned) gridHalves / 2;
	keepLargest(geometry);
}

static void announceGeometry(void) {
	struct retro_game_geometry changed;
	geometry(&changed);
	environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &changed);
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	gridAnnounce = false;
	gbOnAdvance = gameBoy && !gbTogether && libretroGbOnAdvance();
	geometry(&info->geometry);
	// A Game Boy frame lasts as long as a Game Boy Advance frame.
	info->timing.fps = FRAME_RATE;
	info->timing.sample_rate = AUDIO_RATE;
}

void retro_init(void) {
	cheatsInit(&cheats);
	linkSetEnvironment(environment);
	linkSetSaveHook(saveReceived);
}

void retro_deinit(void) {
	free(correctedColors);
	correctedColors = NULL;
	colorCorrection = false;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
}

// Options.

// Builds the color correction table from Pokefan531's measurements of the screen: its primaries
// mixed in linear light, at a gamma of 2.2.
static uint16_t corrected(uint16_t color) {
	return correctedColors[(color & 0x1F) | ((color >> 1) & 0x7FE0)];
}

static bool buildCorrectedColors(void) {
	if (correctedColors) {
		return true;
	}
	correctedColors = malloc(0x8000 * sizeof(*correctedColors));
	if (!correctedColors) {
		return false;
	}
	static const float mix[3][3] = {
		{ 0.80f, 0.275f, -0.075f },
		{ 0.135f, 0.64f, 0.225f },
		{ 0.195f, 0.155f, 0.65f },
	};
	float linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = powf(i / 31.0f, 2.2f);
	}
	for (int i = 0; i < 0x8000; ++i) {
		float in[3] = { linear[i >> 10], linear[(i >> 5) & 0x1F], linear[i & 0x1F] };
		int out[3];
		for (int c = 0; c < 3; ++c) {
			float value = mix[c][0] * in[0] + mix[c][1] * in[1] + mix[c][2] * in[2];
			value = value < 0 ? 0 : value > 1 ? 1 : value;
			out[c] = (int) (powf(value, 1 / 2.2f) * (c == 1 ? 63 : 31) + 0.5f);
		}
		correctedColors[i] = out[0] << 11 | out[1] << 5 | out[2];
	}
	return true;
}

// Names each button for the loaded console, including the tilt sensor's controls.
static void showButtons(void) {
	bool const forGameBoy = gameBoy;
	static struct retro_input_descriptor shown[80];
	size_t n = 0;
#define BUTTON(device, index, id, name) shown[n++] = (struct retro_input_descriptor) { port, device, index, id, name }
	unsigned port = 0;
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A");
	BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B");
	if (TRIGGERS_TURBO) {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Turbo L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Turbo R");
	} else if (togetherRunning()) {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, !l2FastForward ? "Previous Layout" : triggersToggle ? "Fast-Forward Toggle (Previous Layout in Netplay)" : "Fast-Forward (Previous Layout in Netplay)");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Next Screens");
	} else {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, triggersToggle ? "Fast-Forward Toggle" : "Fast-Forward");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, triggersToggle ? "Fast-Forward Toggle" : "Fast-Forward");
	}
	if (forGameBoy) {
		// A Game Boy has no L or R; on the Game Boy Advance model either stretches the picture, as on
		// that console. With a tilt cartridge, L is the flick.
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, tiltCartridge ? "Flick (Motion Sensor)" : "Stretch (Game Boy Advance Model)");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Stretch (Game Boy Advance Model)");
		if (replayHasButton() && replayButton != REPLAY_BUTTON_OFF) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, replayButton, "Cheat Cartridge Button");
		}
	} else {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L");
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R");
		if (replayHasButton() && replayButton != REPLAY_BUTTON_OFF) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, replayButton, "Cheat Cartridge Button");
		}
	}
	if (l3Free()) {
		BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, l3Button == L3_FAST_FORWARD ? "Fast-Forward Toggle" : "Soft Reset (Hold)");
	}
	if (tiltControl != TILT_DPAD && !(tiltControl == TILT_DEVICE && forGameBoy)) {
		unsigned const stick = tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X, "Tilt / Turn (Motion Sensor)");
		BUTTON(RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y,
		       !forGameBoy && tiltControl == TILT_RIGHT_STICK ? "Tilt Up / Down, Sunlight" : "Tilt Up / Down (Motion Sensor)");
	}
	if (!forGameBoy && tiltControl != TILT_RIGHT_STICK) {
		BUTTON(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Sunlight (Solar Sensor)");
	}
	if (forGameBoy && contrastStick()) {
		BUTTON(RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Contrast");
		if (contrastClick()) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Contrast Reset");
		}
	}
	for (port = 1; port < (unsigned) playersHere; ++port) {
		static const unsigned ids[12] = {
			RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_RIGHT,
			RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
			RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
		};
		for (int i = 0; i < 12; ++i) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, ids[i], shown[i].description);
		}
		if (!forGameBoy) {
			BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L");
			BUTTON(RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R");
		}
	}
#undef BUTTON
	shown[n] = (struct retro_input_descriptor) { 0, 0, 0, 0, NULL };
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, shown);
}

static void readFrameSkip(void);

// The display's size, where the system says it (Linux's DRM: the first connected screen's first mode),
// wider than it is tall, as a handheld is held to play.
static bool displaySize(unsigned* width, unsigned* height) {
	DIR* const drm = opendir("/sys/class/drm");
	if (!drm) {
		return false;
	}
	bool found = false;
	for (struct dirent* e; !found && (e = readdir(drm));) {
		if (!strchr(e->d_name, '-')) {
			continue;
		}
		char path[300], line[64] = "";
		snprintf(path, sizeof(path), "/sys/class/drm/%s/status", e->d_name);
		FILE* f = fopen(path, "r");
		bool const connected = f && fgets(line, sizeof(line), f) && !strncmp(line, "connected", 9);
		if (f) {
			fclose(f);
		}
		snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", e->d_name);
		f = connected ? fopen(path, "r") : NULL;
		unsigned w = 0, h = 0;
		found = f && fgets(line, sizeof(line), f) && sscanf(line, "%ux%u", &w, &h) == 2 && w && h;
		if (f) {
			fclose(f);
		}
		if (found) {
			*width = w > h ? w : h;
			*height = w > h ? h : w;
		}
	}
	closedir(drm);
	return found;
}

// Screen Size for Fit Screen: the frontend's, else the display's own (else 1280x720), one listed, or
// Custom's, set as hundreds, tens and even ones so any size can be picked from short lists.
static void readScreenSize(unsigned* width, unsigned* height) {
	const char* const size = option("armgba_lcd_screen");
	unsigned w = 1280, h = 720;
	if ((!size || !strcmp(size, "auto")) && libretroLcdScreenWidth && libretroLcdScreenHeight) {
		w = libretroLcdScreenWidth;
		h = libretroLcdScreenHeight;
	} else if (!size || !strcmp(size, "auto")) {
		displaySize(&w, &h);
	} else if (!strcmp(size, "custom")) {
		const char* const width[3] = { option("armgba_lcd_width_hundreds"), option("armgba_lcd_width_tens"), option("armgba_lcd_width_ones") };
		const char* const height[3] = { option("armgba_lcd_height_hundreds"), option("armgba_lcd_height_tens"), option("armgba_lcd_height_ones") };
		w = (width[0] ? (unsigned) atoi(width[0]) : 12) * 100 + (width[1] ? (unsigned) atoi(width[1]) : 80) + (width[2] ? (unsigned) atoi(width[2]) : 0);
		h = (height[0] ? (unsigned) atoi(height[0]) : 7) * 100 + (height[1] ? (unsigned) atoi(height[1]) : 20) + (height[2] ? (unsigned) atoi(height[2]) : 0);
	} else {
		sscanf(size, "%ux%u", &w, &h);
	}
	*width = w >= 160 && w <= 4096 ? w : 1280;
	*height = h >= 144 && h <= 4096 ? h : 720;
}

// The Game Boy Action Replay's switch (Action Replay Switch), which the cartridge reads all the time.
static bool replaySwitch = true;

static void applyGenieCodes(void);

static void readOptions(void) {
	replaySwitch = !optionIs("armgba_action_replay_switch", "off");
	applyGenieCodes();
	// In netplay the button only comes from the controller, which every device sees alike; the menu's
	// options are each device's own.
	bool const hold = optionIs("armgba_action_replay_hold", "on");
	if (hold != replayHoldOption) {
		replayHoldOption = hold;
		if (!netplaySeen) {
			controls.replayHold = hold;
		}
	}
	// In netplay what the stick clicks and combos do is the host's, which its states bring (see
	// retro_unserialize); this device's options wait until a game is loaded again.
	bool const toggles = !optionIs("armgba_action_replay_press_mode", "hold");
	unsigned const button = optionIs("armgba_action_replay_button", "off")  ? REPLAY_BUTTON_OFF
	                        : optionIs("armgba_action_replay_button", "l3") ? RETRO_DEVICE_ID_JOYPAD_L3
	                                                                        : RETRO_DEVICE_ID_JOYPAD_R3;
	if (!netplaySeen) {
		replayToggles = toggles;
	}
	if (button != replayButton && !netplaySeen) {
		replayButton = button;
		if (actionReplayLoaded) {
			showButtons();
		}
	}
	static const struct Choice threadedChoices[] = { { "enabled", THREADED_ON }, { "one frame late", THREADED_LATE } };
	static const struct Choice modelChoices[] = {
		{ "Game Boy", LIBRETRO_GB_DMG },
		{ "Game Boy Pocket", LIBRETRO_GB_MGB },
		{ "Game Boy Color", LIBRETRO_GB_CGB },
		{ "Game Boy Advance", LIBRETRO_GB_AGB },
	};
	static const struct Choice bootChoices[] = { { "disabled", LIBRETRO_GB_BOOT_OFF }, { "open-source", LIBRETRO_GB_BOOT_ARMGBA } };
	// Before the measured screens, "original" and "pocket" were brighter greens; they get the measured ones.
	static const struct Choice shadeChoices[] = {
		{ "grayscale", 0 }, { "measured", 1 }, { "original", 1 }, { "pocket measured", 2 }, { "pocket", 2 }, { "light", 3 },
	};
	static const struct Choice screensChoices[] = {
		{ "top and bottom", SCREENS_STACKED },
		{ "player 1", SCREENS_ONE },
		{ "player 2", SCREENS_ONE + 1 },
		{ "player 3", SCREENS_ONE + 2 },
		{ "player 4", SCREENS_ONE + 3 },
	};
	static const struct Choice restartChoices[] = {
		{ "l r a b", RESTART_L_R_A_B },
		{ "l3 r3", RESTART_STICKS },
		{ "select start a b", RESTART_SELECT_START_A_B },
		{ "off", RESTART_OFF },
	};
	leftStickDpad = optionIs("armgba_left_stick_dpad", "enabled");
	static const struct Choice tiltChoices[] = { { "right stick", TILT_RIGHT_STICK }, { "d-pad", TILT_DPAD }, { "device", TILT_DEVICE } };

	skipBios = optionIs("armgba_skip_bios", "enabled");
	const char* clock = option("armgba_cpu_clock");
	int percent = clock ? atoi(clock) : 100;
	// In netplay, the host's, which its states bring (see retro_unserialize)
	if (!netplaySeen) {
		cpuOverclock = percent >= 25 && percent <= 400 && percent % 25 == 0 ? percent / 25 - 4 : 0;
		stockDuringBoot = !optionIs("armgba_cpu_clock_boot", "clock");
	}
	const char* address = option("armgba_gamecube_address");
	dolphinSetHost(gamecubeWanted() && !gameBoy ? (address ? address : "auto") : NULL);
	int threaded = optionChoice("armgba_threaded_video", threadedChoices, COUNT(threadedChoices), THREADED_OFF);
	// Linked consoles run in turns, which a render thread would only slow.
	threaded = gbaTogether ? THREADED_OFF : threaded;
	int const threads = optionIs("armgba_video_threads", "2") ? 2 : 1;
	if (gba && (threaded != threadedVideo || (threaded && threads != videoThreads))) {
		renderThreadStop(gba);
		if (threaded) {
			renderThreadStart(gba, threaded == THREADED_LATE, threads == 2);
		}
	}
	threadedVideo = threaded;
	// The same goes for a sound thread, which serves one console.
	bool const audioThread = !gbaTogether && optionIs("armgba_threaded_audio", "enabled");
	if (gba && audioThread != threadedAudio) {
		if (audioThread) {
			audioThreadStart(gba);
		} else {
			audioThreadStop(gba);
		}
	}
	threadedAudio = audioThread;
	videoThreads = threads;
	colorCorrection = optionIs("armgba_color_correction", "enabled") && buildCorrectedColors();
	// LCD Effects off leaves each effect's setting as it is, and off.
	bool const effects = optionIs("armgba_lcd_effects", "enabled");
	static const struct Choice ghostingChoices[] = { { "25", 1 }, { "half", 2 }, { "75", 3 }, { "enabled", 4 } };
	ghosting = effects ? optionChoice("armgba_lcd_ghosting", ghostingChoices, COUNT(ghostingChoices), 0) : 0;
	static const struct Choice gridChoices[] = {
		{ "2", 4 }, { "2.5", 5 }, { "3", 6 }, { "3.5", 7 }, { "4", 8 }, { "4.5", 9 }, { "5", 10 }, { "5.5", 11 }, { "6", 12 }, { "6.5", 13 }, { "7", 14 }, { "7.5", 15 }, { "8", 16 },
	};
	int const grid = effects ? optionChoice("armgba_lcd_grid", gridChoices, COUNT(gridChoices), 2) : 2;
	gridAnnounce |= grid != gridHalves;
	gridHalves = grid;
	bool const fitting = effects && optionIs("armgba_lcd_grid", "fit");
	unsigned fitWidth = gridFitWidth, fitHeight = gridFitHeight;
	if (fitting) {
		readScreenSize(&fitWidth, &fitHeight);
	}
	gridAnnounce |= fitting != gridFit || fitWidth != gridFitWidth || fitHeight != gridFitHeight;
	gridFit = fitting;
	gridFitWidth = fitWidth;
	gridFitHeight = fitHeight;
	static const struct Choice wayChoices[] = {
		{ "up", 1 }, { "up right", 2 }, { "right", 3 }, { "down right", 4 }, { "down", 5 }, { "down left", 6 }, { "left", 7 }, { "up left", 8 },
	};
	gridWay = optionChoice("armgba_lcd_shadow", wayChoices, COUNT(wayChoices), 0);
	gridGaps = optionIs("armgba_lcd_gaps", "enabled");
	// "0.5" to "2.5", read in halves so the locale's decimal point doesn't matter.
	const char* const distance = option("armgba_lcd_shadow_distance");
	const char* const point = distance ? strchr(distance, '.') : NULL;
	int const halves = distance ? atoi(distance) * 2 + (point && point[1] == '5') : 3;
	libretroLcdShadowDistance = (halves >= 1 && halves <= 5 ? halves : 3) / 2.0f;
	const char* const subpixels = option("armgba_lcd_subpixels");
	libretroLcdSubpixelMix = subpixels ? fminf(fmaxf(atoi(subpixels) / 100.0f, 0), 1) : 0;
	static const struct Choice threadChoices[] = { { "1", 1 }, { "2", 2 }, { "3", 3 }, { "4", 4 } };
	lcdThreads = optionChoice("armgba_lcd_threads", threadChoices, COUNT(threadChoices), 0);
	readFrameSkip();
	const char* const filter = option("armgba_low_pass_filter");
	lowPass = filter ? atoi(filter) * 0x10000 / 100 : 0;
	gbModel = optionChoice("armgba_gb_model", modelChoices, COUNT(modelChoices), LIBRETRO_GB_AUTO);
	libretroGbSetBootRom(optionChoice("armgba_gb_boot_rom", bootChoices, COUNT(bootChoices), LIBRETRO_GB_BOOT_NINTENDO));
	int const shades = optionChoice("armgba_gb_screen", shadeChoices, COUNT(shadeChoices), 0);
	gbShades = shades;
	const char* const contrast = option("armgba_gb_contrast");
	gbContrast = contrast ? atoi(contrast) : 0;
	gbColorCorrection = optionIs("armgba_gb_color_correction", "enabled");
	gbColorPanel = optionIs("armgba_gb_advance_panel", "color");
	interleaved = effects && optionIs("armgba_lcd_interleave", "enabled");
	for (int i = gameBoy ? playersHere - 1 : 0; i >= 0; --i) {
		libretroGbSelect(i);
		libretroGbSetContrast(gbContrast);
		libretroGbSetScreen(shades);
		libretroGbSetColorPanel(gbColorPanel);
		if (gameBoy) {
			libretroGbSetColorCorrection(gbColorCorrection);
		}
	}
	libretroGbSetPrinter(optionIs("armgba_link_mode", "off") && optionIs("armgba_gb_printer", "on"), printoutPage);
	int const chosenScreens = optionChoice("armgba_link_screens", screensChoices, COUNT(screensChoices), SCREENS_SIDE);
	// R2 (and L2 in netplay) changes the screens too; the option wins only when it changes.
	if (chosenScreens != screensOption) {
		screensOption = screens = chosenScreens;
		if (togetherRunning()) {
			announceGeometry();
		}
	}
	static const struct Choice switchChoices[] = {
		{ "l r a b", SWITCH_L_R_A_B },
		{ "l3 r3", SWITCH_L3_R3 },
		{ "start l2 r2", SWITCH_START_L2_R2 },
		{ "start l3", SWITCH_START_L3 },
		{ "start r3", SWITCH_START_R3 },
		{ "off", SWITCH_OFF },
	};
	switchController = optionChoice("armgba_switch_controller", switchChoices, COUNT(switchChoices), SWITCH_START_L2_R2);
	static const struct Choice sharedChoices[] = {
		{ "everyone", SHARED_EVERYONE },
		{ "pass around", SHARED_PASS },
	};
	// Only with Link Mode off: the link modes give each player a console.
	const char* const link = option("armgba_link_mode");
	oneConsole = !link || !strcmp(link, "off");
	sharedControls = oneConsole ? optionChoice("armgba_shared_controls", sharedChoices, COUNT(sharedChoices), SHARED_OFF) : SHARED_OFF;
	const char* const follows = option("armgba_r2_follows");
	r2Follows = follows && *follows >= '1' && *follows <= '4' ? *follows - '1' : -1;
	playerNumbers = optionIs("armgba_player_numbers", "enabled");
	bool const fastForward = !optionIs("armgba_link_l2", "previous screens");
	if (fastForward != l2FastForward) {
		l2FastForward = fastForward;
		if (togetherRunning()) {
			showButtons();
		}
	}
	bool const cropped = !optionIs("armgba_gb_advance_border", "shown");
	if (cropped != gbCropped) {
		gbCropped = cropped;
		if (gameBoy && gbOnAdvance) {
			announceGeometry();
		}
	}
	bool const triggers = optionIs("armgba_l2_r2", "turbo");
	bool const toggle = optionIs("armgba_l2_r2", "toggle");
	if (toggle != triggersToggle) {
		triggersToggle = toggle;
		if (gba || gameBoy) {
			showButtons();
		}
	}
	static const struct Choice l3Choices[] = { { "fast forward", L3_FAST_FORWARD }, { "soft reset", L3_SOFT_RESET }, { "off", L3_OFF } };
	int const l3 = optionChoice("armgba_l3_button", l3Choices, COUNT(l3Choices), L3_FAST_FORWARD);
	int const restart = optionChoice("armgba_restart_buttons", restartChoices, COUNT(restartChoices), RESTART_L_R_START);
	// In netplay, the host's (see retro_unserialize)
	if (!netplaySeen && (triggers != turboOnTriggers || l3 != l3Button || restart != restartButtons)) {
		turboOnTriggers = triggers;
		l3Button = l3;
		restartButtons = restart;
		if (gba || gameBoy) {
			showButtons();
		}
	}
	if (!netplaySeen) {
		const char* const period = option("armgba_turbo_period");
		turboPeriod = clampInt(period ? atoi(period) : TURBO_FEWEST, TURBO_FEWEST, TURBO_MOST);
		turboPulse = clampInt(turboPeriod / 2, 2, 15);
	}
	linkSetGbExact(optionIs("armgba_gb_link_timing", "exact"));
	static const struct Choice rotationChoices[] = { { "90", 1 }, { "180", 2 }, { "270", 3 } };
	motionRotation = optionChoice("armgba_motion_rotation", rotationChoices, COUNT(rotationChoices), 0);
	int const tilt = optionChoice("armgba_gb_tilt", tiltChoices, COUNT(tiltChoices), TILT_LEFT_STICK);
	if (tilt != tiltControl) {
		tiltControl = tilt;
		if (gba || gameBoy) {
			showButtons();
		}
	}
	const char* const value = option("armgba_solar_sensor");
	if (value) {
		solarLevel = atoi(value);
	}
}

// Player 2's Game.

// Lists the games beside the loaded one as Player 2's Game's choices.
static void offerSecondGames(const char* path, bool forGameBoy) {
	gamesFree(secondGames, secondCount);
	secondCount = path ? gamesList(path, forGameBoy, secondGames, SECOND_CHOICES) : 0;
	// A game in an archive comes as "game.zip#game.gb"; the file is the archive.
	snprintf(firstPath, sizeof(firstPath), "%s", path ? path : "");
	firstPath[strcspn(firstPath, "#")] = '\0';
	if (!coreOptions || !secondOption) {
		return;
	}
	size_t n = 0;
	secondOption->values[n++] = (struct retro_core_option_value) { "same", "Same Game" };
	if (!forGameBoy) {
		secondOption->values[n++] = (struct retro_core_option_value) { "none", "No Cartridge" };
	}
	for (size_t i = 0; i < secondCount; ++i) {
		secondOption->values[n++] = (struct retro_core_option_value) { secondGames[i].path, secondGames[i].label };
	}
	secondOption->values[n] = (struct retro_core_option_value) { NULL, NULL };
	fillSaveChoices();
	setOptions();
}

// The file remembering each game's Player 2's Game: in the save directory, or beside player 1's
// game.
static void secondMemoryFile(char* out, size_t outSize) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (saveDirectory && *saveDirectory) {
		snprintf(out, outSize, "%s/armGBA player 2 games.txt", saveDirectory);
		return;
	}
	snprintf(out, outSize, "%s", firstPath);
	char* slash = strrchr(out, '/');
	snprintf(slash ? slash + 1 : out, outSize - (slash ? (size_t) (slash + 1 - out) : 0), "armGBA player 2 games.txt");
}

static bool offered(const char* value) {
	for (size_t i = 0; value && i < secondCount; ++i) {
		if (!strcmp(value, secondGames[i].path)) {
			return true;
		}
	}
	return value && (!strcmp(value, "same") || (!gameBoy && !strcmp(value, "none")));
}

// Whether the other consoles are to start with no cartridge, waiting for player 1's game to send
// itself over the cable.
static bool bareWanted(void) {
	const char* value = option("armgba_player2_game");
	return !pairFile && !gameBoy && value && !strcmp(value, "none");
}

static void recallSecondChoice(void) {
	char memory[PATH_SIZE];
	char choice[PATH_SIZE];
	secondMemoryFile(memory, sizeof(memory));
	if (choiceRecall(memory, firstPath, choice, sizeof(choice)) && offered(choice)) {
		struct retro_variable variable = { "armgba_player2_game", choice };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
	}
}

static void rememberSecondChoice(void) {
	const char* value = option("armgba_player2_game");
	if (*firstPath && offered(value)) {
		char memory[PATH_SIZE];
		secondMemoryFile(memory, sizeof(memory));
		choiceRemember(memory, firstPath, value);
	}
}

// The game chosen for player 2, or NULL for the same game.
static const char* chosenSecond(void) {
	const char* value = pairFile ? NULL : option("armgba_player2_game");
	for (size_t i = 0; value && i < secondCount; ++i) {
		if (!strcmp(value, secondGames[i].path)) {
			return secondGames[i].path;
		}
	}
	return NULL;
}

const char* gameOf(int player) {
	const char* chosen = player == 1 ? secondPath : player > 1 ? otherPaths[player] : "";
	return *chosen ? chosen : firstPath;
}

// The CRC of the BIOS read from the system folder, 0 when armGBA's runs.
static uint32_t biosCrc;

// Nintendo's gba_bios.bin from the system directory, or else armGBA's own BIOS, as BIOS picks.
static bool loadBios(struct GBA* machine) {
	const char* directory = NULL;
	uint8_t bios[SIZE_BIOS];
	bool read = false;
	if (!optionIs("armgba_bios", "open-source") && environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) && directory) {
		char path[PATH_SIZE];
		snprintf(path, sizeof(path), "%s/gba_bios.bin", directory);
		FILE* f = fopen(path, "rb");
		read = f && fread(bios, 1, sizeof(bios), f) == sizeof(bios);
		if (f) {
			fclose(f);
		}
	}
	biosCrc = read ? (uint32_t) crc32(0, bios, sizeof(bios)) : 0;
	if (!read) {
		logMessage(RETRO_LOG_INFO, "armGBA: using armGBA's BIOS");
		return gbaLoadBios(machine, armgbaBios, SIZE_BIOS);
	}
	return gbaLoadBios(machine, bios, sizeof(bios));
}

uint32_t libretroFirmwareCrc(const char** option) {
	*option = gameBoy ? "Game Boy Boot ROM" : "BIOS";
	return gameBoy ? libretroGbBootCrc() : biosCrc;
}

// The clock the consoles run on now.
static int8_t clockNow(void) {
	return booting && stockDuringBoot ? 0 : cpuOverclock;
}

// Restarts a console, on the CPU Clock chosen.
static void resetAdvance(struct GBA* machine) {
	booting = !skipBios;
	machine->cpu.overclock = clockNow();
	gbaReset(machine, skipBios);
}

// Puts the consoles on the CPU Clock chosen, when it changed or a state from another was loaded
// (loaded).
static void applyCpuClock(bool loaded) {
	libretroGbSetCpuClock(4 + clockNow(), loaded);
	for (int i = 0; !gameBoy && i < playersHere; ++i) {
		gbaSetOverclock(gbas[i], clockNow());
	}
}

// Whether a console is still booting: a Game Boy in its boot ROM, a Game Boy Advance before its BIOS
// set POSTFLG, as it does just before starting the cartridge or a program another console sent (and
// as skipping the BIOS does). Only the BIOS sets it and nothing clears it, so the console's state
// tells, unlike where the CPU is: a game waits for VBlank in the BIOS.
static bool stillBooting(void) {
	bool still = gameBoy && libretroGbBooting();
	for (int i = 0; !gameBoy && i < playersHere; ++i) {
		still |= !(gbas[i]->io[IO(REG_POSTFLG)] & 1);
	}
	return still;
}

// Once the game has started, the CPU Clock applies.
static void checkBooted(void) {
	if (booting && !stillBooting()) {
		booting = false;
		applyCpuClock(false);
	}
}

static void* readPatched(const char* path, size_t* size);

// With Low Memory, the frontend leaves the game to the core, which maps it from its file unless it
// has a patch to apply.
static bool loadCartridge(struct GBA* machine, const struct retro_game_info* game) {
	bool const fastSaves = optionIs("armgba_fast_flash_saves", "enabled");
	if (game->data) {
		bool const loaded = gbaLoadRom(machine, game->data, game->size);
		if (loaded && fastSaves) {
			savedataSkipFlashTimer(machine);
		}
		return loaded;
	}
	if (!patchesBeside(game->path) && gbaMapRom(machine, game->path)) {
#if defined(__3DS__)
		// Read into memory there. Elsewhere the file is mapped, and stays as it is.
		if (fastSaves) {
			savedataSkipFlashTimer(machine);
		}
#endif
		return true;
	}
	size_t size = 0;
	void* data = readPatched(game->path, &size);
	bool const loaded = data && gbaLoadRom(machine, data, size);
	free(data);
	if (loaded && fastSaves) {
		savedataSkipFlashTimer(machine);
	}
	return loaded;
}

static struct GBA* startAdvance(const struct retro_game_info* game) {
	struct GBA* machine = gbaCreate();
	if (!machine) {
		return NULL;
	}
	// RetroArch loads the save file into save memory after this, and writes it back from there.
	machine->savedata.backed = true;
	machine->renderer.rgb565 = true;
	if (!loadBios(machine) || !loadCartridge(machine, game)) {
		gbaDestroy(machine);
		return NULL;
	}
	return machine;
}

// Starts the other players' Game Boys; RetroArch's own netplay runs them all.
static bool startOtherGameBoys(int count, const void* const data[], const size_t sizes[]) {
	const char* directory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
	char error[256];
	bool const adapter = adapterWanted(count);
	libretroGbSetPlayers(count, adapter);
	int loaded = 1;
	while (loaded < count) {
		libretroGbSelect(loaded);
		if (!libretroGbLoad(data[loaded], sizes[loaded], directory, gbModel, gbColorCorrection, AUDIO_RATE, error, sizeof(error))) {
			break;
		}
		++loaded;
	}
	if (loaded < count) {
		for (int i = loaded; i > 0; --i) {
			libretroGbSelect(i);
			libretroGbUnload();
		}
		libretroGbSelect(0);
		libretroGbSetPlayers(1, false);
		showMessage(error);
		return false;
	}
	libretroGbSelect(0);
	gbTogether = true;
	gbAdapter = adapter;
	playersHere = count;
	linkSetGameBoy(false);
	return true;
}

static void stopOtherGameBoys(void) {
	for (int i = playersHere - 1; i > 0; --i) {
		libretroGbSelect(i);
		libretroGbUnload();
	}
	libretroGbSelect(0);
	libretroGbSetPlayers(1, false);
	gbTogether = false;
	gbAdapter = false;
	playersHere = 1;
	linkSetGameBoy(true);
}

// Plugs the GBA Link Port option's choice into each console: the Wireless Adapter, or the cable
// between the consoles here.
static void attachPorts(void) {
	const char* value = option("armgba_link_port");
	bool wireless = value && !strcmp(value, "wireless") && (togetherWanted() || separateWanted());
	linkSetWireless(wireless && !gbaTogether);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (!gbas[i]) {
			continue;
		}
		airs[i] = (struct RfuAir) { i, false, airSend };
		rfuAttach(gbas[i], wireless, gbaTogether ? &airs[i] : linkAir());
		if (gbaTogether) {
			sioAttach(gbas[i], wireless ? NULL : &cables[i]);
		}
	}
}

static void setAdvanceOffsets(void) {
	static const int32_t behind[MAX_PLAYERS] = { 0, GBA_FRAME_CYCLES / 2, GBA_FRAME_CYCLES / 4, GBA_FRAME_CYCLES * 3 / 4 };
	memcpy(gbaAhead, behind, sizeof(gbaAhead));
}

static bool startOtherAdvances(int count, const void* const data[], const size_t sizes[]) {
	for (int i = 1; i < count; ++i) {
		struct retro_game_info game = { NULL, data[i], sizes[i], NULL };
		gbas[i] = startAdvance(&game);
		if (!gbas[i]) {
			for (int k = i - 1; k > 0; --k) {
				gbaDestroy(gbas[k]);
				gbas[k] = NULL;
			}
			return false;
		}
	}
	if (threadedVideo) {
		renderThreadStop(gba);
		threadedVideo = 0;
	}
	audioThreadStop(gba);
	threadedAudio = false;
	gbaTogether = true;
	playersHere = count;
	linkSetGba(NULL);
	for (int i = 0; i < count; ++i) {
		if (i) {
			resetAdvance(gbas[i]);
		}
		cables[i] = (struct SioLink) { .context = (void*) (intptr_t) i, .players = count, .id = i, .begin = cableBegin, .finish = cableFinish, .wrote = cableWrote };
	}
	attachPorts();
	setAdvanceOffsets();
	workersStart(count);
	return true;
}

static void stopOtherAdvances(void) {
	workersStop();
	for (int i = 1; i < MAX_PLAYERS; ++i) {
		if (gbas[i]) {
			gbaDestroy(gbas[i]);
		}
		gbas[i] = NULL;
	}
	gbaTogether = false;
	playersHere = 1;
	sioAttach(gba, NULL);
	linkSetGba(gba);
	attachPorts();
}

// A game the core reads itself, patched as RetroArch patches the games it reads (patch.h).
static void* readPatched(const char* path, size_t* size) {
	void* data = gameRead(path, size);
	char name[256];
	if (data && patchGame(path, &data, size, name, sizeof(name)) < 0) {
		char message[320];
		snprintf(message, sizeof(message), "armGBA: %.250s doesn't fit this game, which runs unpatched", name);
		showMessage(message);
	}
	return data;
}

// The game each other player's console runs: player 2's from the frontend, the menu or a pair file,
// players 3 and 4's from a pair file, or else player 1's. Returns false if one can't be read.
static bool readOtherGames(const struct retro_game_info* second, const void* sameData, size_t sameSize, int count,
                           const void* data[], size_t sizes[], void* read[]) {
	for (int i = 1; i < count; ++i) {
		const char* path = i == 1 ? secondPath : otherPaths[i];
		if (i == 1 && second && second->data) {
			data[i] = second->data;
			sizes[i] = second->size;
		} else if (othersBare) {
			data[i] = "";
			sizes[i] = 0;
		} else if (!*path && sameData) {
			data[i] = sameData;
			sizes[i] = sameSize;
		} else {
			data[i] = read[i] = readPatched(*path ? path : firstPath, &sizes[i]);
		}
		if (!data[i]) {
			char message[300];
			snprintf(message, sizeof(message), "armGBA can't read player %d's game", i + 1);
			showMessage(message);
			return false;
		}
	}
	return true;
}

static void announcePlayers(void) {
	char message[300];
	const char* name = strrchr(secondPath, '/');
	if (othersBare) {
		snprintf(message, sizeof(message), "%d players, %s with no cartridge", playersHere, playersHere > 2 ? "the others" : "player 2");
	} else if (playersHere > 2) {
		snprintf(message, sizeof(message), "%d players", playersHere);
	} else {
		snprintf(message, sizeof(message), "Player 2: %.240s", *secondPath ? (name ? name + 1 : secondPath) : "the same game, with its own save");
	}
	notify(message, 240);
}

// Starts the other players' consoles beside player 1's: two to four in all with Together, as
// many as a pair file names, or two with the game the frontend loaded for player 2.
static void startOthers(const struct retro_game_info* second, const void* sameData, size_t sameSize) {
	secondFromFrontend = second && !pairFile;
	othersBare = !second && bareWanted();
	int const count = pairFile ? pairCount : second ? 2 : playersWanted();
	if (!pairFile) {
		const char* choice = second ? NULL : chosenSecond();
		snprintf(secondPath, sizeof(secondPath), "%s", second && second->path ? second->path : choice ? choice : "");
	}
	const void* data[MAX_PLAYERS] = { NULL };
	size_t sizes[MAX_PLAYERS] = { 0 };
	void* read[MAX_PLAYERS] = { NULL };
	bool started = readOtherGames(pairFile ? NULL : second, sameData, sameSize, count, data, sizes, read);
	if (started) {
		started = gameBoy ? startOtherGameBoys(count, data, sizes) : startOtherAdvances(count, data, sizes);
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		free(read[i]);
	}
	if (!started) {
		secondPath[0] = '\0';
		return;
	}
	if (!secondFromFrontend) {
		readOtherSaves();
	}
	readOptions();
	readClockOptions(false);
	announcePlayers();
}

static void stopOthers(void) {
	writeCoreSaves(false);
	forgetOtherSaves();
	if (gbTogether) {
		stopOtherGameBoys();
	}
	if (gbaTogether) {
		stopOtherAdvances();
	}
	secondPath[0] = '\0';
	secondFromFrontend = false;
	othersBare = false;
}

// Started with no game: a Game Boy Advance whose cartridge stays out, with no save, and Save Manager.
static bool noGame;

bool noGameLoaded(void) {
	return noGame;
}

// Offers RetroArch the netplay that carries the cable when Link Mode is Separate or there is no game,
// and its usual netplay otherwise.
static void offerLinkNetplay(void) {
	linkSetNetplay((separateWanted() || noGame) && !togetherRunning());
}

static bool runningAsWanted(void) {
	bool const want = togetherWanted();
	if (togetherRunning() != want) {
		return false;
	}
	if (!want) {
		return true;
	}
	const char* choice = chosenSecond();
	bool same = !strcmp(choice ? choice : "", secondPath) && playersHere == playersWanted();
	same &= !gameBoy || gbAdapter == adapterWanted(playersHere);
	same &= othersBare == bareWanted();
	return same;
}

// Restart takes in a change of Link Mode, Number of Players, a Link Port or Player 2's Game.
static void applyLinkOptions(void) {
	if (secondFromFrontend || pairFile) {
		return;
	}
	rememberSecondChoice();
	if (runningAsWanted()) {
		return;
	}
	if (togetherRunning()) {
		stopOthers();
	}
	if (togetherWanted()) {
		startOthers(NULL, NULL, 0);
	}
	announceGeometry();
	showOptions();
	showButtons();
}

// Describes memory for the frontend's achievements and cheat search, the same way mGBA's and
// gambatte-libretro's cores do.
static void announceMemory(void) {
	struct retro_memory_descriptor descriptors[11];
	size_t count = 0;
	if (gameBoy) {
		count = libretroGbMemoryMap(descriptors);
		gbMapColor = libretroGbIsColor();
	}
	if (!count && gba) {
		size_t romSize = gba->romSize + (gba->romSize & 1);
		const struct retro_memory_descriptor advance[] = {
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->iwram, 0, 0x03000000, 0xFF000000, 0, SIZE_IWRAM, NULL },
			{ RETRO_MEMDESC_SYSTEM_RAM, gba->ewram, 0, 0x02000000, 0xFF000000, 0, SIZE_EWRAM, NULL },
			{ 0, gba->savedata.storage, 0, 0x0E000000, 0, 0, saveSize(gba), NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x08000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0A000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->rom, 0, 0x0C000000, 0, 0, romSize, NULL },
			{ RETRO_MEMDESC_CONST, gba->bios, 0, 0x00000000, 0, 0, SIZE_BIOS, NULL },
			{ 0, gba->vram, 0, 0x06000000, 0xFF000000, 0, SIZE_VRAM, NULL },
			{ 0, gba->palette, 0, 0x05000000, 0xFF000000, 0, SIZE_PALETTE, NULL },
			{ 0, gba->oam, 0, 0x07000000, 0xFF000000, 0, SIZE_OAM, NULL },
			{ 0, gba->io, 0, 0x04000000, 0, 0, SIZE_IO, NULL },
		};
		count = COUNT(advance);
		memcpy(descriptors, advance, sizeof(advance));
	}
	struct retro_memory_map map = { descriptors, count };
	environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool yes = true;
	environment(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
}

// A restart or a loaded state can switch the machine to or from a Game Boy Color, which changes the
// memory map.
static void checkGameBoyMemory(void) {
	if (gbMapColor != libretroGbIsColor()) {
		announceMemory();
	}
}

// Game Boy and Game Boy Color games are told apart from Game Boy Advance games by file extension.
static bool hasGameBoyExtension(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

// A Game Boy Advance game: it starts on an ARM branch over its header (xx 00 00 EA), or a branch and
// the header's fixed 96 at B2, or the start of the Nintendo logo at 4. Of 4,599 Game Boy Advance files
// (No-Intro's, hacks, homebrew, prototypes, BIOSes) all but a DS expansion cartridge pass; none of 38
// Mega Duck games does, as they start on Game Boy code (F3 di, C3 jp).
static bool looksAdvance(const uint8_t* rom) {
	static const uint8_t logo[] = { 0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21 };
	return (rom[3] == 0xEA && rom[2] == 0 && rom[1] == 0) || (rom[3] == 0xEA && rom[0xB2] == 0x96) || !memcmp(rom + 4, logo, sizeof(logo));
}

// A Mega Duck game: -1 if it isn't one, else the kind of cartridge for libretroGbSetMegaDuck. Its own
// extensions name the kind (.duck none, .md0, .md1, .md2, .md20s, .md25s, as megaduck-info and Super
// Junior SameDuck have them). A .bin, as some Game Boy Advance games are too, is a Mega Duck's when it
// isn't a Game Boy Advance game and has no Game Boy's Nintendo logo at 104; its cartridges hold 32 KiB
// to 2 MiB (16 KiB for some made since).
static int megaDuckKind(const struct retro_game_info* game) {
	static const char* const kinds[] = { ".duck", ".md0", ".md1", ".md2", ".md20s", ".md25s" };
	const char* dot = game->path ? strrchr(game->path, '.') : NULL;
	if (!dot || !game->data) {
		return -1;
	}
	for (int i = 0; i < (int) (sizeof(kinds) / sizeof(kinds[0])); ++i) {
		if (!strcasecmp(dot, kinds[i])) {
			return game->size >= 0x150 && game->size <= 0x800000 ? 1 + i : -1;
		}
	}
	if (strcasecmp(dot, ".bin") || game->size < 0x4000 || game->size > 0x200000) {
		return -1;
	}
	static const uint8_t logo[] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B };
	const uint8_t* rom = game->data;
	return !looksAdvance(rom) && memcmp(rom + 0x104, logo, sizeof(logo)) ? 0 : -1;
}

static bool isMegaDuck(const struct retro_game_info* game) {
	return megaDuckKind(game) >= 0;
}

static bool loadMegaDuckLaptop(const struct retro_game_info* system, const struct retro_game_info* slot);

static void RETRO_CALLCONV keyboardEvent(bool down, unsigned keycode, uint32_t character, uint16_t modifiers) {
	if (megaDuckGame) {
		libretroGbKeyboard(down, keycode, character, modifiers);
	}
}

// The Mega Duck laptop's options, for the game about to load: Auto puts the laptop on for its system
// ROM and cartridges (.md0)
static bool readMegaDuckOptions(int kind, bool forceOn) {
	const char* const laptop = option("armgba_megaduck_laptop");
	bool const on = forceOn || (laptop && !strcmp(laptop, "enabled") ? true : laptop && !strcmp(laptop, "disabled") ? false : kind == 2);
	static const struct Choice printerChoices[] = { { "none", 0 }, { "one pass", 1 }, { "two pass", 2 } };
	static const struct Choice speechChoices[] = { { "spanish", 0 }, { "german", 1 } };
	libretroGbSetMegaDuckLaptop(on, optionChoice("armgba_megaduck_printer", printerChoices, COUNT(printerChoices), 2),
	                            optionChoice("armgba_megaduck_speech", speechChoices, COUNT(speechChoices), 0));
	if (on) {
		struct retro_keyboard_callback keyboard = { keyboardEvent };
		environment(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);
	}
	return on;
}

// Mega Duck Laptop Cartridge: the Mega Duck cartridges beside the laptop program, remembered for each
// program as Player 2's Game is
static void slotMemoryFile(char* out, size_t outSize) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (saveDirectory && *saveDirectory) {
		snprintf(out, outSize, "%s/armGBA laptop cartridges.txt", saveDirectory);
		return;
	}
	snprintf(out, outSize, "%s", slotFirst);
	char* slash = strrchr(out, '/');
	snprintf(slash ? slash + 1 : out, outSize - (slash ? (size_t) (slash + 1 - out) : 0), "armGBA laptop cartridges.txt");
}

static const char* chosenSlot(void) {
	const char* value = option("armgba_megaduck_slot");
	for (size_t i = 0; value && i < slotCount; ++i) {
		if (!strcmp(value, slotGames[i].path)) {
			return slotGames[i].path;
		}
	}
	return NULL;
}

static void offerSlotGames(const char* path) {
	gamesFree(slotGames, slotCount);
	slotCount = path ? gamesList(path, GAMES_MEGA_DUCK, slotGames, SECOND_CHOICES) : 0;
	snprintf(slotFirst, sizeof(slotFirst), "%s", path ? path : "");
	slotFirst[strcspn(slotFirst, "#")] = '\0';
	if (!coreOptions || !slotOption) {
		return;
	}
	size_t n = 0;
	slotOption->values[n++] = (struct retro_core_option_value) { "none", "None" };
	for (size_t i = 0; i < slotCount; ++i) {
		slotOption->values[n++] = (struct retro_core_option_value) { slotGames[i].path, slotGames[i].label };
	}
	slotOption->values[n] = (struct retro_core_option_value) { NULL, NULL };
	setOptions();
	char memory[PATH_SIZE];
	char choice[PATH_SIZE];
	slotMemoryFile(memory, sizeof(memory));
	if (choiceRecall(memory, slotFirst, choice, sizeof(choice))) {
		for (size_t i = 0; i < slotCount; ++i) {
			if (!strcmp(choice, slotGames[i].path)) {
				struct retro_variable variable = { "armgba_megaduck_slot", choice };
				environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
			}
		}
	}
}

static void rememberSlotChoice(void) {
	const char* value = option("armgba_megaduck_slot");
	if (*slotFirst && value && (!strcmp(value, "none") || chosenSlot())) {
		char memory[PATH_SIZE];
		slotMemoryFile(memory, sizeof(memory));
		choiceRemember(memory, slotFirst, value);
	}
}

// Action Replay Cartridge: the game on top of an Action Replay, from the Game Boy Advance games
// beside it, those played most recently first, remembered for each Action Replay. The subsystem
// "Cheat Cartridge (Game on Top)" takes any game.
static struct ListedGame replayGames[SECOND_CHOICES];
static size_t replayCount;
static char replayFirst[PATH_SIZE];
// The game on top, for its save and the subsystem's.
static char replayGamePath[PATH_SIZE];
// Whether the Action Replay loaded has a game on top.
static bool replayHasGame;
static const struct retro_game_info* replayGameGiven;

// Before, the game on top was kept in this file, for every cheat cartridge; it's read for a
// cartridge whose save doesn't say.
static void replayMemoryFile(char* out, size_t outSize) {
	const char* saveDirectory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory);
	if (saveDirectory && *saveDirectory) {
		snprintf(out, outSize, "%s/armGBA action replay games.txt", saveDirectory);
		return;
	}
	snprintf(out, outSize, "%s", replayFirst);
	char* slash = strrchr(out, '/');
	snprintf(slash ? slash + 1 : out, outSize - (slash ? (size_t) (slash + 1 - out) : 0), "armGBA action replay games.txt");
}

static const char* chosenReplayGame(void) {
	const char* value = option("armgba_action_replay_game");
	for (size_t i = 0; value && i < replayCount; ++i) {
		if (!strcmp(value, replayGames[i].path)) {
			return replayGames[i].path;
		}
	}
	return NULL;
}

// How recently a game was played: its save's time, or 0 without one. Other cheat cartridges come last.
static long long replayRank(const char* path, void* context) {
	(void) context;
	static const char* const carts[] = { "Action Replay", "Game Genie", "GameShark", "Xploder", "CodeBreaker", "Code Breaker" };
	for (size_t i = 0; i < sizeof(carts) / sizeof(*carts); ++i) {
		if (strstr(path, carts[i])) {
			return -1;
		}
	}
	char file[SAVE_PATH_SIZE];
	char rtc[SAVE_PATH_SIZE];
	saveFilesFor(path, file, rtc);
	struct stat info;
	return stat(file, &info) ? 0 : (long long) info.st_mtime;
}

// The game on top is kept in the cheat cartridge's save, after what the cartridge itself keeps (its
// flash chip or RAM; nothing for the Game Genie): this mark, the game's file name (empty for none) and
// a newline. A save without it is from before, or has no game chosen yet.
static const char replayTopMark[] = "\nROM Inserted: ";
static char replayTop[512];
static bool replayTopKept;
// The Game Genie's save then names the cheats chosen for its code screen, a line each:
// "Game Genie Code 1: " and the codes, as RetroArch's cheat database writes them.
static const char genieMark[] = "Game Genie Code ";
enum { GENIE_SLOTS = 3, GENIE_CODE = sizeof(((struct ChtCheat*) 0)->code) };
static char genieKept[GENIE_SLOTS][GENIE_CODE];
// Whether the cheat cartridge's save file is one armGBA can't tell, which isn't written for the game
// on top alone.
static bool replaySaveForeign;

// Reads the game on top (and the Game Genie's cheats) from the end of the save at file. Returns where
// that starts (the file's size without it), or -1 with no file.
static long readReplayTop(const char* file) {
	replayTopKept = false;
	memset(genieKept, 0, sizeof(genieKept));
	FILE* f = fopen(file, "rb");
	if (!f) {
		return -1;
	}
	enum { MARK = sizeof(replayTopMark) - 1, MOST = MARK + sizeof(replayTop) + GENIE_SLOTS * (sizeof(genieMark) + 4 + GENIE_CODE) };
	char tail[MOST + 1];
	long size = fseek(f, 0, SEEK_END) ? 0 : ftell(f);
	long const from = size > MOST ? size - MOST : 0;
	size_t const n = size > 0 && !fseek(f, from, SEEK_SET) ? fread(tail, 1, (size_t) (size - from), f) : 0;
	fclose(f);
	// The last mark: then lines of text, the first the game's name, that end the file
	for (size_t i = n > MARK ? n - MARK : 0; n > MARK && i-- > 0;) {
		if (memcmp(&tail[i], replayTopMark, MARK)) {
			continue;
		}
		char* const lines = &tail[i + MARK];
		size_t const length = n - i - MARK;
		if (tail[n - 1] != '\n' || memchr(lines, '\0', length)) {
			break;
		}
		tail[n] = '\0';
		char* const end = strchr(lines, '\n');
		size_t const nameLength = (size_t) (end - lines);
		if (nameLength >= sizeof(replayTop) || memchr(lines, '/', nameLength)) {
			break;
		}
		memcpy(replayTop, lines, nameLength);
		replayTop[nameLength] = '\0';
		replayTopKept = true;
		for (char* line = end + 1; *line;) {
			char* const next = strchr(line, '\n');
			*next = '\0';
			int slot = 0;
			int used = 0;
			if (!strncmp(line, genieMark, sizeof(genieMark) - 1) && sscanf(line + sizeof(genieMark) - 1, "%d: %n", &slot, &used) == 1 &&
			    used && slot >= 1 && slot <= GENIE_SLOTS) {
				snprintf(genieKept[slot - 1], GENIE_CODE, "%s", line + sizeof(genieMark) - 1 + used);
			}
			line = next + 1;
		}
		return from + (long) i;
	}
	return size;
}

static const char* fileName(const char* path) {
	const char* const slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void offerReplayGames(const char* path) {
	gamesFree(replayGames, replayCount);
	replayCount = path ? gamesListRanked(path, gameBoy ? GAMES_GAME_BOY : GAMES_ADVANCE, replayRank, NULL, replayGames, SECOND_CHOICES) : 0;
	snprintf(replayFirst, sizeof(replayFirst), "%s", path ? path : "");
	replayFirst[strcspn(replayFirst, "#")] = '\0';
	if (!coreOptions || !replayOption) {
		return;
	}
	size_t n = 0;
	replayOption->values[n++] = (struct retro_core_option_value) { "none", "None" };
	for (size_t i = 0; i < replayCount; ++i) {
		replayOption->values[n++] = (struct retro_core_option_value) { replayGames[i].path, replayGames[i].label };
	}
	replayOption->values[n] = (struct retro_core_option_value) { NULL, NULL };
	setOptions();
	// RetroArch drops the option's value as the core starts, when the list holds only None, so the
	// core keeps it: in the cartridge's save, or before, in the file for all of them.
	char save[SAVE_PATH_SIZE];
	char rtc[SAVE_PATH_SIZE];
	saveFilesFor(path, save, rtc);
	readReplayTop(save);
	char memory[PATH_SIZE];
	char choice[PATH_SIZE];
	replayMemoryFile(memory, sizeof(memory));
	bool const before = !replayTopKept && choiceRecall(memory, replayFirst, choice, sizeof(choice));
	for (size_t i = 0; (replayTopKept || before) && i < replayCount; ++i) {
		if (replayTopKept ? !strcmp(replayTop, fileName(replayGames[i].path)) : !strcmp(choice, replayGames[i].path)) {
			struct retro_variable variable = { "armgba_action_replay_game", replayGames[i].path };
			environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
		}
	}
}

// Game Genie Code 1-3: the Game Genie cheats RetroArch's cheat database has for the game on top.
static struct ChtCheat genieCheats[RETRO_NUM_CORE_OPTION_VALUES_MAX - 2];
static size_t genieCount;

size_t genieCheatCount(void) {
	return genieCount;
}

// The cheat a Game Genie Code option has chosen, or NULL.
static const char* genieChosen(int slot) {
	char key[32];
	snprintf(key, sizeof(key), "armgba_game_genie_code_%d", slot + 1);
	const char* const value = option(key);
	for (size_t i = 0; value && i < genieCount; ++i) {
		if (!strcmp(value, genieCheats[i].code)) {
			return genieCheats[i].code;
		}
	}
	return NULL;
}

// The chosen cheats' codes go to the Game Genie, in order, one slot each, as far as three go.
static void applyGenieCodes(void) {
	// In netplay the codes are the host's, which its states bring; each device's options are its own.
	if (!gameGenieLoaded || netplaySeen) {
		return;
	}
	uint8_t digits[GENIE_SLOTS * 9];
	uint8_t counts[GENIE_SLOTS] = { 0 };
	memset(digits, 0x10, sizeof(digits));
	int slot = 0;
	for (int i = 0; i < GENIE_SLOTS; ++i) {
		const char* const chosen = genieChosen(i);
		char codes[GENIE_CODE];
		snprintf(codes, sizeof(codes), "%s", chosen ? chosen : "");
		for (char* code = strtok(codes, "+"); code && slot < GENIE_SLOTS; code = strtok(NULL, "+")) {
			if (!chtGameGenieCode(code)) {
				continue;
			}
			int n = 0;
			for (const char* c = code; *c; ++c) {
				if (*c != '-') {
					digits[slot * 9 + n++] = (uint8_t) (isdigit((unsigned char) *c) ? *c - '0' : toupper((unsigned char) *c) - 'A' + 10);
				}
			}
			counts[slot++] = (uint8_t) n;
		}
	}
	libretroGbSetGameGenieCodes(digits, counts);
}

// The standalone app's copy of RetroArch's cheat database (see link.h), and an import waiting for it
static int (*cheatFetch)(const char* gamePath, bool advance, bool start, char* out, size_t outSize);
static bool importWaiting;

void libretroSetCheatFetch(int (*fetch)(const char* gamePath, bool advance, bool start, char* out, size_t outSize)) {
	cheatFetch = fetch;
}

// The Game Genie with a game on top: its code options list the database's cheats for the game, and
// take those its save names.
static void offerGenieCheats(void) {
	genieCount = 0;
	const char* system = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system);
	char database[PATH_SIZE];
	// RetroArch's, or the app's copy if it has one already
	if (gameGenieLoaded && *replayGamePath &&
	    (chtDatabase(system, database, sizeof(database)) ||
	     (cheatFetch && cheatFetch(replayGamePath, false, false, database, sizeof(database)) == CHEAT_FETCH_READY))) {
		genieCount = chtCheats(database, false, replayGamePath, chtGameGenieCode, genieCheats, COUNT(genieCheats));
	}
	for (int i = 0; coreOptions && i < GENIE_SLOTS; ++i) {
		struct retro_core_option_v2_definition* const definition = genieOptions[i];
		if (!definition) {
			continue;
		}
		size_t n = 0;
		definition->values[n++] = (struct retro_core_option_value) { "none", "None" };
		for (size_t k = 0; k < genieCount; ++k) {
			definition->values[n++] = (struct retro_core_option_value) { genieCheats[k].code, genieCheats[k].description };
		}
		definition->values[n] = (struct retro_core_option_value) { NULL, NULL };
	}
	if (coreOptions) {
		setOptions();
	}
	for (int i = 0; i < GENIE_SLOTS; ++i) {
		for (size_t k = 0; *genieKept[i] && k < genieCount; ++k) {
			if (!strcmp(genieKept[i], genieCheats[k].code)) {
				char key[32];
				snprintf(key, sizeof(key), "armgba_game_genie_code_%d", i + 1);
				struct retro_variable variable = { key, genieCheats[k].code };
				environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
			}
		}
	}
	applyGenieCodes();
}

static void writeReplayFlash(void);

// Import Cheats from RetroArch, into the Xploder GB's list in its flash chip from 0x20000 (see
// XploderGb): for each game its name, its number of cheats, and for each cheat its name, its number
// of codes and the codes, 4 bytes each (type, value, address low, address high, as a GameShark code
// writes them); an empty name ends it. Its program takes up to 64 KiB of it, 63 codes a cheat, and
// 0x0AAA rows in its list (a row for each game, cheat and code, and a NEW one after each).
enum {
	XPLODER_CHEATS = 0x20000,
	XPLODER_ROOM = 0x10000,
	XPLODER_ROWS = 0x0AAA,
	XPLODER_CODES = 63,
	XPLODER_NAME = 17,
};

// The end of the string at at, within end, or 0.
static size_t xploderSkipName(const uint8_t* list, size_t at, size_t end) {
	while (at < end && list[at]) {
		++at;
	}
	return at < end ? at + 1 : 0;
}

// A name as the Xploder shows it: printable characters, as many as fit.
static void xploderName(const char* from, char* out) {
	size_t n = 0;
	for (; *from && n < XPLODER_NAME; ++from) {
		out[n++] = *from >= 0x20 && *from < 0x7F ? *from : '?';
	}
	while (n && out[n - 1] == ' ') {
		--n;
	}
	out[n] = '\0';
}

// Whether the codes at a and b (count each) are the same, in any order.
static bool xploderSameCodes(const uint8_t* a, size_t count, const uint8_t* b, size_t bCount) {
	if (count != bCount) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		bool found = false;
		for (size_t k = 0; k < count && !found; ++k) {
			found = !memcmp(&a[i * 4], &b[k * 4], 4);
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

// The later Game Boy Action Replays (Online, Xtreme) and the GameShark Online, as LibreShark's
// sharkdumps documents their ROM (imhex-gbc-gs-rom-pattern): at 8000 a count, then up to 455 games
// of 18 bytes (a 12-bit number counted from 1, flags above it, and a name of 16, padded with
// spaces); at A000, C000 and E000 a count, then up to 455 codes of 18 bytes each (the game's number,
// with 10 above it while the code is on, a name of 12 and the 4 bytes of a GameShark code). A cheat
// of several codes is as many codes of one name.
enum {
	AR_GAMES = 0x8000,
	AR_ENTRY = 18,
	AR_SLOTS = 455,
	AR_GAME_NAME = 16,
	AR_CODE_NAME = 12,
};

static const size_t arCodeBanks[] = { 0xA000, 0xC000, 0xE000 };

static unsigned arCount(const uint8_t* flash, size_t at) {
	unsigned const count = flash[at] | flash[at + 1] << 8;
	return count <= AR_SLOTS ? count : 0;
}

static void arName(const char* from, uint8_t* out, size_t size) {
	size_t n = 0;
	for (; *from && n < size; ++from) {
		out[n++] = (uint8_t) (*from >= 0x20 && *from < 0x7F ? *from : '?');
	}
	memset(&out[n], ' ', size - n);
}

static bool arSameName(const uint8_t* a, const uint8_t* b, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		if (toupper(a[i]) != toupper(b[i])) {
			return false;
		}
	}
	return true;
}

// Returns how many cheats went in, or -1 when the list can't be read.
static int arGbImport(uint8_t* flash, size_t size, const char* title, const struct ChtCheat* cheats, size_t count, size_t* already, size_t* left) {
	*already = *left = 0;
	if (size < 0x10000 || (flash[AR_GAMES] == 0xFF && flash[AR_GAMES + 1] == 0xFF)) {
		return -1;
	}
	unsigned games = arCount(flash, AR_GAMES);
	uint8_t name[AR_GAME_NAME];
	arName(title, name, sizeof(name));
	unsigned id = 0;
	unsigned most = 0;
	for (unsigned i = 0; i < games; ++i) {
		const uint8_t* const game = &flash[AR_GAMES + 2 + i * AR_ENTRY];
		unsigned const number = game[0] | (game[1] & 0x0F) << 8;
		most = number > most ? number : most;
		if (!id && arSameName(&game[2], name, sizeof(name))) {
			id = number;
		}
	}
	bool const newGame = !id;
	if (newGame) {
		if (games >= AR_SLOTS || most >= 0xFFF) {
			*left = count;
			return 0;
		}
		id = most + 1;
	}
	int added = 0;
	for (size_t i = 0; i < count; ++i) {
		uint8_t codes[64][4];
		size_t n = 0;
		char copy[sizeof(cheats[i].code)];
		snprintf(copy, sizeof(copy), "%s", cheats[i].code);
		bool fits = true;
		for (char* code = strtok(copy, "+"); code && fits; code = strtok(NULL, "+")) {
			fits = n < COUNT(codes) && chtGameSharkCode(code);
			for (int b = 0; fits && b < 4; ++b) {
				unsigned value;
				sscanf(&code[b * 2], "%2x", &value);
				codes[n][b] = (uint8_t) value;
			}
			n += fits;
		}
		if (!fits || !n) {
			continue;
		}
		// Already the game's, if each of its codes is, under any name
		size_t have = 0;
		for (size_t c = 0; c < n; ++c) {
			bool found = false;
			for (size_t b = 0; b < COUNT(arCodeBanks) && !found; ++b) {
				unsigned const codesThere = arCount(flash, arCodeBanks[b]);
				for (unsigned k = 0; k < codesThere && !found; ++k) {
					const uint8_t* const entry = &flash[arCodeBanks[b] + 2 + k * AR_ENTRY];
					found = (entry[0] | (entry[1] & 0x0F) << 8) == id && !memcmp(&entry[14], codes[c], 4);
				}
			}
			have += found;
		}
		if (have == n) {
			++*already;
			continue;
		}
		size_t room = 0;
		for (size_t b = 0; b < COUNT(arCodeBanks); ++b) {
			room += AR_SLOTS - arCount(flash, arCodeBanks[b]);
		}
		if (room < n) {
			++*left;
			continue;
		}
		if (newGame && !added) {
			uint8_t* const game = &flash[AR_GAMES + 2 + games * AR_ENTRY];
			game[0] = (uint8_t) id;
			game[1] = (uint8_t) (id >> 8 & 0x0F);
			memcpy(&game[2], name, sizeof(name));
			++games;
			flash[AR_GAMES] = (uint8_t) games;
			flash[AR_GAMES + 1] = (uint8_t) (games >> 8);
		}
		for (size_t c = 0; c < n; ++c) {
			for (size_t b = 0; b < COUNT(arCodeBanks); ++b) {
				unsigned const there = arCount(flash, arCodeBanks[b]);
				if (there >= AR_SLOTS) {
					continue;
				}
				uint8_t* const entry = &flash[arCodeBanks[b] + 2 + there * AR_ENTRY];
				entry[0] = (uint8_t) id;
				entry[1] = (uint8_t) (id >> 8 & 0x0F);
				arName(cheats[i].description, &entry[2], AR_CODE_NAME);
				memcpy(&entry[14], codes[c], 4);
				flash[arCodeBanks[b]] = (uint8_t) (there + 1);
				flash[arCodeBanks[b] + 1] = (uint8_t) ((there + 1) >> 8);
				break;
			}
		}
		++added;
	}
	return added;
}

// The first Game Boy Action Replay (the Pro Action Replay) keeps four codes in its RAM, from 4A, 4
// bytes each in a GameShark code's order, all 0 for none. Its program sets 40 to 1 as it first
// starts, clearing them; until then they'd be cleared.
enum { AR_FIRST_RAM = 0x1000, AR_FIRST_SET_UP = 0x40, AR_FIRST_CODES = 0x4A, AR_FIRST_SLOTS = 4 };

// Returns how many cheats went in its empty slots, all of each one's codes or none.
static int arFirstImport(uint8_t* ram, size_t size, const struct ChtCheat* cheats, size_t count, size_t* already, size_t* left) {
	*already = *left = 0;
	if (size != AR_FIRST_RAM) {
		return -1;
	}
	if (ram[AR_FIRST_SET_UP] != 1) {
		*left = count;
		return -2;
	}
	uint8_t* const slots = &ram[AR_FIRST_CODES];
	int added = 0;
	for (size_t i = 0; i < count; ++i) {
		uint8_t codes[AR_FIRST_SLOTS][4];
		size_t n = 0;
		char copy[sizeof(cheats[i].code)];
		snprintf(copy, sizeof(copy), "%s", cheats[i].code);
		bool fits = true;
		for (char* code = strtok(copy, "+"); code && fits; code = strtok(NULL, "+")) {
			fits = chtGameSharkCode(code);
			for (int b = 0; fits && n < AR_FIRST_SLOTS && b < 4; ++b) {
				unsigned value;
				sscanf(&code[b * 2], "%2x", &value);
				codes[n][b] = (uint8_t) value;
			}
			n += fits;
		}
		if (!fits || !n) {
			continue;
		}
		// More codes than it has slots: it can't take this one
		if (n > AR_FIRST_SLOTS) {
			++*left;
			continue;
		}
		// Each of its codes in a slot already, once however many slots hold it
		size_t have = 0;
		size_t empty = 0;
		for (size_t c = 0; c < n; ++c) {
			bool found = false;
			for (int k = 0; k < AR_FIRST_SLOTS && !found; ++k) {
				found = !memcmp(&slots[k * 4], codes[c], 4);
			}
			have += found;
		}
		for (int k = 0; k < AR_FIRST_SLOTS; ++k) {
			static const uint8_t none[4];
			empty += !memcmp(&slots[k * 4], none, 4);
		}
		if (have >= n) {
			++*already;
			continue;
		}
		if (empty < n) {
			++*left;
			continue;
		}
		for (size_t c = 0, k = 0; c < n; ++k) {
			static const uint8_t none[4];
			if (!memcmp(&slots[k * 4], none, 4)) {
				memcpy(&slots[k * 4], codes[c++], 4);
			}
		}
		++added;
	}
	return added;
}

// Whether the Game Boy Action Replay loaded is the first one (see isGameBoyActionReplay)
static bool replayFirstKind;

static bool cheatImportOn(void) {
	bool const takes = actionReplayLoaded && !gameGenieLoaded;
	return takes && replayHasGame && *replayGamePath;
}

bool cheatImportable(void) {
	return cheatImportOn();
}

// Returns how many cheats went in, or -1 when the list can't be read; *already counts those it had
// and *left those that didn't fit.
static int xploderImport(uint8_t* flash, size_t size, const char* title, const struct ChtCheat* cheats, size_t count, size_t* already, size_t* left) {
	*already = *left = 0;
	if (size < XPLODER_CHEATS + XPLODER_ROOM) {
		return -1;
	}
	uint8_t* const list = &flash[XPLODER_CHEATS];
	char name[XPLODER_NAME + 1];
	xploderName(title, name);
	// The list as it is: where the game is, where its cheats end, and where the list ends
	size_t at = 0;
	size_t game = SIZE_MAX;
	size_t gameCheats = 0;
	size_t rows = 1;
	while (at < XPLODER_ROOM && list[at]) {
		size_t const start = at;
		if (!(at = xploderSkipName(list, at, XPLODER_ROOM)) || at >= XPLODER_ROOM) {
			return -1;
		}
		bool const ours = game == SIZE_MAX && !strcasecmp((const char*) &list[start], name);
		size_t const countAt = at++;
		rows += 2;
		for (unsigned c = 0; c < list[countAt]; ++c) {
			if (!(at = xploderSkipName(list, at, XPLODER_ROOM)) || at >= XPLODER_ROOM) {
				return -1;
			}
			rows += 2 + list[at];
			at += 1 + 4u * list[at];
			if (at > XPLODER_ROOM) {
				return -1;
			}
		}
		if (ours) {
			game = start;
			gameCheats = at;
		}
	}
	if (at >= XPLODER_ROOM) {
		return -1;
	}
	size_t const listEnd = at;
	// The new list, the cheats added at the end of the game's own (or a new game's)
	uint8_t* const out = malloc(XPLODER_ROOM);
	if (!out) {
		return -1;
	}
	size_t const before = game == SIZE_MAX ? listEnd : gameCheats;
	memcpy(out, list, before);
	size_t used = before;
	size_t countAt = SIZE_MAX;
	if (game == SIZE_MAX) {
		size_t const length = strlen(name);
		if (!length || used + length + 3 > XPLODER_ROOM || rows + 2 > XPLODER_ROWS) {
			free(out);
			*left = count;
			return 0;
		}
		memcpy(&out[used], name, length + 1);
		used += length + 1;
		countAt = used;
		out[used++] = 0;
		rows += 2;
	} else {
		countAt = xploderSkipName(list, game, XPLODER_ROOM);
	}
	int added = 0;
	for (size_t i = 0; i < count; ++i) {
		// Its codes, as the Xploder keeps them
		uint8_t codes[XPLODER_CODES * 4];
		size_t n = 0;
		char copy[sizeof(cheats[i].code)];
		snprintf(copy, sizeof(copy), "%s", cheats[i].code);
		bool fits = true;
		for (char* code = strtok(copy, "+"); code && fits; code = strtok(NULL, "+")) {
			fits = n < XPLODER_CODES && chtGameSharkCode(code);
			for (int b = 0; fits && b < 4; ++b) {
				unsigned value;
				sscanf(&code[b * 2], "%2x", &value);
				codes[n * 4 + b] = (uint8_t) value;
			}
			n += fits;
		}
		if (!fits || !n) {
			continue;
		}
		// Already the game's, under any name?
		bool known = false;
		for (size_t c = countAt + 1, k = 0; game != SIZE_MAX && k < list[countAt] && !known; ++k) {
			c = xploderSkipName(list, c, XPLODER_ROOM);
			known = xploderSameCodes(codes, n, &list[c + 1], list[c]);
			c += 1 + 4u * list[c];
		}
		for (size_t c = countAt + 1 + (game != SIZE_MAX ? gameCheats - countAt - 1 : 0), k = 0; k < (size_t) added && !known; ++k) {
			c = xploderSkipName(out, c, used);
			known = xploderSameCodes(codes, n, &out[c + 1], out[c]);
			c += 1 + 4u * out[c];
		}
		if (known) {
			++*already;
			continue;
		}
		char cheatName[XPLODER_NAME + 1];
		xploderName(cheats[i].description, cheatName);
		size_t const length = strlen(cheatName);
		size_t const need = length + 2 + n * 4;
		if (out[countAt] == 0xFF || used + need + (listEnd - before) + 1 > XPLODER_ROOM || rows + 2 + n > XPLODER_ROWS) {
			++*left;
			continue;
		}
		memcpy(&out[used], cheatName, length + 1);
		used += length + 1;
		out[used++] = (uint8_t) n;
		memcpy(&out[used], codes, n * 4);
		used += n * 4;
		++out[countAt];
		rows += 2 + n;
		++added;
	}
	// The games after it, then the end
	memcpy(&out[used], &list[before], listEnd - before);
	used += listEnd - before;
	out[used++] = 0;
	memset(&out[used], 0xFF, XPLODER_ROOM - used);
	// Nothing added, the list stays as it was (without a new game of no cheats)
	if (added) {
		memcpy(list, out, XPLODER_ROOM);
	}
	free(out);
	return added;
}

static bool cheatRestartDue;

// Whether the Action Replay loaded is one InterAct sold as a GameShark (the GameShark GBA, and the
// GameShark Online, which is the Action Replay Online's program), by its file's name: its ROM
// names itself an Action Replay.
static bool replayGameShark;
// The CRC-32 of the cheat cartridge's file as loaded, 0 with none: states say which they were made with
static uint32_t replayCartCrc;

static bool namesGameShark(const char* path) {
	static const char brand[] = "gameshark";
	const char* const name = path ? fileName(path) : "";
	for (const char* at = name; *at; ++at) {
		size_t i = 0;
		for (const char* c = at; brand[i] && *c; ++c) {
			if (*c == ' ' || *c == '-' || *c == '_') {
				continue;
			}
			if (tolower((unsigned char) *c) != brand[i]) {
				break;
			}
			++i;
		}
		if (!brand[i]) {
			return true;
		}
	}
	return false;
}

// The cheat cartridge's name, for its notifications.
static const char* cartridgeName(void) {
	return gameGenieLoaded ? "Game Genie" : fcdLoaded ? fcdLoaded : xploderLoaded ? "Xploder" : replayFirstKind ? "Pro Action Replay" : replayGameShark ? "GameShark" : "Action Replay";
}

static void importCheats(void) {
	const char* system = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system);
	char database[PATH_SIZE];
	if (!chtDatabase(system, database, sizeof(database))) {
		// Without RetroArch's, the app downloads it; the import goes on once it's in (retro_run)
		int const fetched = cheatFetch ? cheatFetch(replayGamePath, !gameBoy, true, database, sizeof(database)) : CHEAT_FETCH_NONE;
		if (fetched == CHEAT_FETCH_BUSY) {
			if (!importWaiting) {
				notify("Downloading RetroArch's cheat database...", 600);
			}
			importWaiting = true;
			return;
		}
		importWaiting = false;
		if (fetched != CHEAT_FETCH_READY) {
			showMessage(cheatFetch ? "armGBA couldn't download RetroArch's cheat database" : "armGBA can't find RetroArch's cheat database");
			return;
		}
	}
	importWaiting = false;
	static struct ChtCheat cheats[1024];
	size_t const count = chtCheats(database, !gameBoy, replayGamePath, gameBoy ? chtGameSharkCode : chtCodeBreakerPart, cheats, COUNT(cheats));
	char title[256];
	snprintf(title, sizeof(title), "%.255s", fileName(replayGamePath));
	char* const dot = strrchr(title, '.');
	if (dot) {
		*dot = '\0';
	}
	title[strcspn(title, "(")] = '\0';
	char* end = title + strlen(title);
	while (end > title && (end[-1] == ' ' || end[-1] == '.')) {
		*--end = '\0';
	}
	char message[300];
	if (!count) {
		snprintf(message, sizeof(message), "RetroArch's cheat database has no %s cheats for %.200s", gameBoy ? "GameShark" : "CodeBreaker", title);
		notify(message, 300);
		logMessage(RETRO_LOG_INFO, message);
		return;
	}
	size_t size;
	uint8_t* const flash = gameBoy ? libretroGbActionReplayFlash(&size) : actionReplayFlash(&size);
	size_t already = 0;
	size_t left = 0;
	const char* const cartridge = cartridgeName();
	int added = -1;
	if (!flash) {
	} else if (!gameBoy) {
		size_t gameSize = 0;
		uint8_t* const game = gameRead(replayGamePath, &gameSize);
		added = !game ? -1
		        : fcdLoaded ? fcdImport(flash, size, game, gameSize, cheats, count, &already, &left)
		        : (added = arGbaImport(flash, size, game, gameSize, cheats, count, &already, &left)) != -4 ? added
		                    : maxImport(flash, size, game, gameSize, cheats, count, &already, &left);
		free(game);
	} else if (xploderLoaded) {
		added = xploderImport(flash, size, title, cheats, count, &already, &left);
	} else if (replayFirstKind) {
		added = arFirstImport(flash, size, cheats, count, &already, &left);
	} else {
		added = arGbImport(flash, size, title, cheats, count, &already, &left);
	}
	if (added == -3) {
		snprintf(message, sizeof(message), "%s: its list has no master code for %.150s, so its codes can't run", cartridge, title);
		notify(message, 300);
		logMessage(RETRO_LOG_INFO, message);
		return;
	}
	if (added == -2) {
		notify("Pro Action Replay: import once its menu shows", 300);
		logMessage(RETRO_LOG_INFO, "Pro Action Replay: import once its menu shows");
		return;
	}
	if (added < 0) {
		snprintf(message, sizeof(message), "armGBA can't read the %s's cheat list", cartridge);
		showMessage(message);
		return;
	}
	if (added) {
		writeReplayFlash();
		cheatRestartDue = true;
	}
	int n = snprintf(message, sizeof(message), "%s: %d cheat%s for %.150s imported", cartridge, added, added == 1 ? "" : "s", title);
	if (already) {
		n += snprintf(message + n, sizeof(message) - (size_t) n, ", %zu already there", already);
	}
	if (left) {
		snprintf(message + n, sizeof(message) - (size_t) n, ", %zu didn't fit", left);
	}
	notify(message, 300);
	logMessage(RETRO_LOG_INFO, message);
}

// What the cheat cartridge's chip held as it loaded, before its save went over it: its ROM, as the core
// starts it (with the Xploder GB's empty cheat list, and the Xploder Advance (Europe) dump's missing
// byte), for Restore Factory Cheats.
static uint8_t* replayFactory;
static size_t replayFactorySize;

static void keepReplayFactory(const uint8_t* flash, size_t size) {
	free(replayFactory);
	replayFactory = flash && size ? malloc(size) : NULL;
	replayFactorySize = replayFactory ? size : 0;
	if (replayFactory) {
		memcpy(replayFactory, flash, size);
	}
}

static bool cheatRestoreOn(void) {
	return actionReplayLoaded && !gameGenieLoaded && replayFactory;
}

bool cheatRestorable(void) {
	return cheatRestoreOn();
}

// Restore Factory Cheats: the chip as it came, the game on top kept; then the console restarts.
static void restoreCheats(void) {
	size_t size;
	uint8_t* const flash = gameBoy ? libretroGbActionReplayFlash(&size) : actionReplayFlash(&size);
	char message[200];
	if (!flash || size != replayFactorySize) {
		snprintf(message, sizeof(message), "armGBA can't restore the %s's cheats", cartridgeName());
		showMessage(message);
		return;
	}
	if (!memcmp(flash, replayFactory, size)) {
		snprintf(message, sizeof(message), "%s: its cheats are as they came already", cartridgeName());
		notify(message, 300);
		logMessage(RETRO_LOG_INFO, message);
		return;
	}
	memcpy(flash, replayFactory, size);
	writeReplayFlash();
	cheatRestartDue = true;
	snprintf(message, sizeof(message), "%s: its cheats are back as they came", cartridgeName());
	notify(message, 300);
	logMessage(RETRO_LOG_INFO, message);
}

static void readCheatRestore(bool act) {
	if (optionIs("armgba_cheat_restore", "restore")) {
		struct retro_variable off = { "armgba_cheat_restore", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		if (act && cheatRestoreOn() && !netplaySeen) {
			restoreCheats();
		}
	}
}

static void readCheatImport(bool act) {
	if (optionIs("armgba_cheat_import", "import")) {
		struct retro_variable off = { "armgba_cheat_import", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		if (act && cheatImportOn() && !netplaySeen) {
			importCheats();
		}
	}
}

static void writeReplayFlash(void);

// The game on top, and the Game Genie's cheats, chosen as the game closes go in the cartridge's save,
// when they changed.
static void rememberReplayChoice(void) {
	bool changed = false;
	const char* value = option("armgba_action_replay_game");
	const char* const chosen = chosenReplayGame();
	if (*replayFirst && value && (!strcmp(value, "none") || chosen)) {
		const char* const name = chosen ? fileName(chosen) : "";
		if ((!replayTopKept || strcmp(name, replayTop)) && strlen(name) < sizeof(replayTop)) {
			snprintf(replayTop, sizeof(replayTop), "%s", name);
			replayTopKept = true;
			changed = true;
		}
	}
	for (int i = 0; gameGenieLoaded && genieCount && i < GENIE_SLOTS; ++i) {
		const char* const code = genieChosen(i);
		if (strcmp(genieKept[i], code ? code : "")) {
			snprintf(genieKept[i], GENIE_CODE, "%s", code ? code : "");
			changed = true;
		}
	}
	if (changed && replayTopKept && !replaySaveForeign) {
		writeReplayFlash();
	}
}

// What the cheat cartridge keeps (its flash chip with its codes and settings, or the Pro Action
// Replay's RAM), then the game on top, is kept by the core where RetroArch would keep the cartridge's
// save. A file of another size, such as a save from before the cartridge was emulated, is left
// alone, so it can't overwrite the program.
static char replayFlashFile[SAVE_PATH_SIZE];
static bool attachActionReplay(const struct retro_game_info* game);
static int replayFlashWait;

// Reads what the cheat cartridge at path keeps (its flash chip, or RAM) over what it starts with.
static void readReplayFlashInto(const char* path, uint8_t* flash, size_t size) {
	keepReplayFactory(flash, size);
	replayGameShark = namesGameShark(path);
	char rtc[SAVE_PATH_SIZE];
	saveFilesFor(path, replayFlashFile, rtc);
	long const kept = readReplayTop(replayFlashFile);
	replaySaveForeign = kept >= 0 && (size_t) kept != size;
	if (flash && size && kept >= 0 && (size_t) kept == size) {
		readFileInto(replayFlashFile, flash, size);
	}
	replayFlashWait = 0;
}

static void readReplayFlash(const char* path) {
	size_t size;
	uint8_t* const flash = actionReplayFlash(&size);
	readReplayFlashInto(path, flash, size);
	actionReplayFlashChanged();
}

static bool replayFlashChanged(void) {
	return gameBoy ? libretroGbActionReplayFlashChanged() : actionReplayFlashChanged();
}

// A device that joined another's netplay session runs that session's Action Replay, which goes with
// its states; its own save stays as it was, as its other saves do.
static void writeReplayFlash(void) {
	if (netplayJoined) {
		replayFlashWait = 0;
		return;
	}
	size_t size;
	const uint8_t* const flash = gameBoy ? libretroGbActionReplayFlash(&size) : actionReplayKept(&size);
	char tail[sizeof(replayTopMark) + sizeof(replayTop) + GENIE_SLOTS * (sizeof(genieMark) + 4 + GENIE_CODE)];
	int top = replayTopKept ? snprintf(tail, sizeof(tail), "%s%s\n", replayTopMark, replayTop) : 0;
	for (int i = 0; top && i < GENIE_SLOTS; ++i) {
		if (*genieKept[i]) {
			top += snprintf(tail + top, sizeof(tail) - (size_t) top, "%s%d: %s\n", genieMark, i + 1, genieKept[i]);
		}
	}
	uint8_t* const save = (flash || !size) && *replayFlashFile && size + (size_t) top ? malloc(size + (size_t) top) : NULL;
	if (save) {
		if (size) {
			memcpy(save, flash, size);
		}
		memcpy(save + size, tail, (size_t) top);
		makeFolders(replayFlashFile);
		writeWholeFile(replayFlashFile, save, size + (size_t) top);
		replaySaveForeign = false;
		free(save);
	}
	replayFlashWait = 0;
}

// Written a second after the program stops changing it, and as the game closes.
static void keepReplayFlash(bool closing) {
	if (replayFlashChanged()) {
		replayFlashWait = 60;
	}
	if (replayFlashWait && (closing || !--replayFlashWait)) {
		writeReplayFlash();
	}
}

// An Action Replay just loaded: the game on top goes in, from the subsystem or Action Replay
// Cartridge. Returns false when it can't be read.
static bool startActionReplay(const struct retro_game_info* game) {
	bool const attached = attachActionReplay(game);
	if (attached) {
		readReplayFlash(game->path ? game->path : firstPath);
	}
	return attached;
}

static bool attachActionReplay(const struct retro_game_info* game) {
	replayGamePath[0] = '\0';
	replayHasGame = false;
	if (replayGameGiven) {
		snprintf(replayGamePath, sizeof(replayGamePath), "%s", replayGameGiven->path ? replayGameGiven->path : "");
		size_t size = replayGameGiven->size;
		void* data = replayGameGiven->data ? NULL : gameRead(replayGameGiven->path, &size);
		replayHasGame = replayGameGiven->data || data;
		bool const attached = actionReplayAttach(gba, replayGameGiven->data ? replayGameGiven->data : data, size);
		free(data);
		return attached;
	}
	offerReplayGames(game->path);
	const char* const chosen = chosenReplayGame();
	if (!chosen) {
		return actionReplayAttach(gba, NULL, 0);
	}
	size_t size = 0;
	void* data = gameRead(chosen, &size);
	if (!data) {
		char message[300];
		snprintf(message, sizeof(message), "armGBA can't read %.250s", chosen);
		showMessage(message);
		return actionReplayAttach(gba, NULL, 0);
	}
	snprintf(replayGamePath, sizeof(replayGamePath), "%s", chosen);
	replayHasGame = true;
	bool const attached = actionReplayAttach(gba, data, size);
	free(data);
	return attached;
}

static bool isPairFile(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && (!strcasecmp(dot, ".gb2") || !strcasecmp(dot, ".gba2"));
}

static bool isGameBoyRom(const void* data, size_t size) {
	// A Game Boy cartridge has the Nintendo logo at 0x104.
	return size >= 0x150 && ((const uint8_t*) data)[0x104] == 0xCE && ((const uint8_t*) data)[0x105] == 0xED;
}

// Datel's Action Replay for the Game Boy and Game Boy Color: no logo for the boot ROM to check (the
// game on top shows it its own), and its registers at 7FE0-7FE7 written all through its program, or
// for the first one, its VBlank vector jumping into its RAM at B000-BFFF and its registers at A800
// and A900 written.
// Whether the last Game Boy Action Replay seen is the first one (the Pro Action Replay, without a
// button or in-game menu).
static bool replayFirstKind;

// Galoob's Game Genie for the Game Boy: its own program of 8 KiB, with no logo for the boot ROM (the
// game's shows), and its name in it (see GameGenieGb).
static bool isGameGenie(const void* data, size_t size) {
	static const char name[] = "GAME GENIE";
	if (!data || size < 0x2000 || size > 0x8000 || isGameBoyRom(data, size)) {
		return false;
	}
	for (size_t i = 0; i + sizeof(name) - 1 <= size; ++i) {
		if (!memcmp((const uint8_t*) data + i, name, sizeof(name) - 1)) {
			return true;
		}
	}
	return false;
}

// Blaze's Xploder GB: its program of 128 KiB, with "Future Console Design" where the boot ROM would
// look for the logo (the game's shows; see XploderGb).
static bool isXploder(const void* data, size_t size) {
	static const char name[] = "Future Console Design";
	return data && size >= 0x150 && size <= 0x40000 && !memcmp((const uint8_t*) data + 0x104, name, sizeof(name) - 1);
}

static bool isGameBoyActionReplay(const void* data, size_t size) {
	if (!data || size < 0x4000 || size > 0x80000 || isGameBoyRom(data, size)) {
		return false;
	}
	const uint8_t* const bytes = data;
	unsigned stores = 0;
	bool showGame = false;
	bool start = false;
	for (size_t i = 0; i + 2 < size; ++i) {
		stores += bytes[i] == 0xEA && bytes[i + 1] >= 0xE0 && bytes[i + 1] <= 0xE7 && bytes[i + 2] == 0x7F;
		showGame |= bytes[i] == 0xEA && bytes[i + 1] == 0x00 && bytes[i + 2] == 0xA8;
		start |= bytes[i] == 0xEA && bytes[i + 1] == 0x00 && bytes[i + 2] == 0xA9;
	}
	bool const first = showGame && start && bytes[0x40] == 0xC3 && bytes[0x42] >= 0xB0 && bytes[0x42] <= 0xBF;
	replayFirstKind = stores < 16 && first;
	return stores >= 16 || first;
}

// The Game Boy Action Replay at game: its program goes to the Game Boy core, and *top becomes the game
// on top to load, from the subsystem or Action Replay Cartridge, or a stand-in for none; *topData is
// what to free once it's loaded.
static bool prepareGameBoyReplay(const struct retro_game_info* game, struct retro_game_info* top, void** topData) {
	static const uint8_t none[0x8000];
	*topData = NULL;
	*top = (struct retro_game_info) { NULL, none, sizeof(none), NULL };
	replayGamePath[0] = '\0';
	const char* chosen = NULL;
	if (replayGameGiven) {
		chosen = replayGameGiven->path;
		if (replayGameGiven->data) {
			*top = *replayGameGiven;
		}
	} else {
		offerReplayGames(game->path);
		chosen = chosenReplayGame();
	}
	if (chosen && top->data == none) {
		size_t size = 0;
		void* const data = gameRead(chosen, &size);
		if (data) {
			*topData = data;
			*top = (struct retro_game_info) { chosen, data, size, NULL };
		} else {
			char message[300];
			snprintf(message, sizeof(message), "armGBA can't read %.250s", chosen);
			showMessage(message);
		}
	}
	if (top->data != none && chosen) {
		snprintf(replayGamePath, sizeof(replayGamePath), "%s", chosen);
	}
	replayHasGame = top->data != none;
	libretroGbSetActionReplay(game->data, game->size, replayHasGame);
	return true;
}

static bool loadGameBoy(const struct retro_game_info* game, const struct retro_game_info* second) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no XRGB8888");
		return false;
	}
	const char* directory = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory);
	offerSecondGames(game->path, true);
	recallSecondChoice();
	gbTogether = false;
	playersHere = 1;
	libretroGbSetPlayers(1, false);
	readOptions();
	readClockOptions(false);
	booting = true;
	libretroGbSetCpuClock(4 + clockNow(), false);
	char error[256];
	libretroGbSelect(0);
	// An Action Replay runs the game on top of it, which the Game Boy core loads as its cartridge.
	gameBoy = true;
	gameGenieLoaded = !isMegaDuck(game) && !second && isGameGenie(game->data, game->size);
	xploderLoaded = !isMegaDuck(game) && !second && isXploder(game->data, game->size);
	fcdLoaded = NULL;
	actionReplayLoaded = gameGenieLoaded || xploderLoaded || (!isMegaDuck(game) && !second && isGameBoyActionReplay(game->data, game->size));
	replayCartCrc = actionReplayLoaded && game->data ? (uint32_t) crc32(0, game->data, (unsigned) game->size) : 0;
	struct retro_game_info top = *game;
	void* topData = NULL;
	if (actionReplayLoaded && !prepareGameBoyReplay(game, &top, &topData)) {
		actionReplayLoaded = gameGenieLoaded = xploderLoaded = false;
	}
	if (!actionReplayLoaded) {
		libretroGbSetActionReplay(NULL, 0, false);
	}
	bool const loaded = libretroGbLoad(top.data, top.size, directory, gbModel, gbColorCorrection, AUDIO_RATE, error, sizeof(error));
	// A tilt cartridge (MBC7), by the header of the game itself, not the Action Replay in front of it
	bool const tilt = !isMegaDuck(game) && top.data && top.size > 0x147 && ((const uint8_t*) top.data)[0x147] == 0x22;
	free(topData);
	if (!loaded) {
		showMessage(error);
		libretroGbUnload();
		gameBoy = false;
		actionReplayLoaded = gameGenieLoaded = xploderLoaded = false;
		return false;
	}
	gbStretched = false;
	if (actionReplayLoaded) {
		// What the Action Replay keeps goes over its program's own, from where RetroArch would keep its save.
		size_t size;
		uint8_t* const kept = libretroGbActionReplayFlash(&size);
		readReplayFlashInto(game->path ? game->path : firstPath, kept, size);
		libretroGbActionReplayFlashChanged();
	}
	replayHint = !actionReplayLoaded ? HINT_NONE : !replayHasGame ? HINT_NO_GAME : gameGenieLoaded ? HINT_GAME_GENIE
	                                                                             : xploderLoaded ? HINT_XPLODER
	                                                                             : replayFirstKind ? HINT_GAME_BOY_FIRST : HINT_GAME_BOY;
	if (gameGenieLoaded) {
		offerGenieCheats();
	}
	if (actionReplayLoaded && *replayGamePath) {
		// The game on top keeps its own save, where it is when the game runs by itself; the core keeps
		// the Action Replay's flash.
		readSave(0, replayGamePath);
	} else if (!actionReplayLoaded) {
		readPlayerOneSave();
	}
	tiltCartridge = tilt;
	linkSetGameBoy(true);
	// The Mega Duck plays alone
	if (!isMegaDuck(game) && (second || togetherWanted() || pairFile)) {
		startOthers(second, game->data, game->size);
	}
	offerLinkNetplay();
	showOptions();
	showButtons();
	announceMemory();
	return true;
}

// Cartridge: with Separate, player 1's console may start with its cartridge out,
// waiting for a game sent over the cable. The game comes back from its file when put back in, its
// save memory as it was; if it was out from the start, RetroArch read no save into it, so the save
// comes from RetroArch's save file once the console has been reset. An Action Replay, with its game
// on top, stays in. Returns whether it went back in.
static bool cartridgeOut;
static bool cartridgeSaveUnread;

static bool applyCartridge(void) {
	const char* value = option("armgba_link_cartridge");
	bool const linked = separateWanted() || gamecubeWanted();
	bool const out = noGame || (!gameBoy && linked && !actionReplayLoaded && value && !strcmp(value, "removed"));
	if (out == cartridgeOut || !gba) {
		return false;
	}
	if (out) {
		gbaLoadRom(gba, "", 0);
		notify(noGame   ? "Save Manager: link in netplay, then Send Save in the core's Saves options"
		       : linked ? "No cartridge: waiting for a game over the cable"
		                : "No cartridge: a game comes over the cable with Link Mode Separate or GameCube",
		       noGame ? 600 : 240);
	} else {
		size_t size;
		void* data = readPatched(firstPath, &size);
		if (!data || !gbaLoadRom(gba, data, size)) {
			free(data);
			showMessage("armGBA can't read the game to put it back in");
			return false;
		}
		free(data);
		savedataForceType(gba, SAVEDATA_AUTODETECT);
	}
	cartridgeOut = out;
	return !out;
}

static void readCartridgeSave(void) {
	if (!cartridgeSaveUnread) {
		return;
	}
	cartridgeSaveUnread = false;
	char file[SAVE_PATH_SIZE], rtcFile[SAVE_PATH_SIZE];
	retroArchFiles(file, rtcFile);
	size_t size;
	readFileInto(file, saveMemory(0, &size), size);
}

static bool loadAdvance(const struct retro_game_info* game, const struct retro_game_info* second) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		logMessage(RETRO_LOG_ERROR, "armGBA: the frontend has no RGB565");
		return false;
	}
	offerSecondGames(game->path, false);
	if (!noGame) {
		recallSecondChoice();
	}
	gbas[0] = gba = startAdvance(game);
	gbaTogether = false;
	playersHere = 1;
	if (!gba) {
		return false;
	}
	gameGenieLoaded = xploderLoaded = replayFirstKind = false;
	actionReplayLoaded = actionReplayDetect(gba->rom, gba->romSize);
	// Before the cartridge fixes its image (the Xploder Advance (Europe) dump's byte)
	replayCartCrc = actionReplayLoaded ? (uint32_t) crc32(0, gba->rom, gba->romSize) : 0;
	fcdLoaded = !fcdDetect(gba->rom, gba->romSize) ? NULL : gba->romSize > 0x10000 ? "Xploder Advance" : "CodeBreaker";
	if (actionReplayLoaded && !startActionReplay(game)) {
		actionReplayLoaded = false;
		fcdLoaded = NULL;
	}
	showButtons();
	threadedVideo = 0;
	threadedAudio = false;
	readOptions();
	readClockOptions(false);
	cartridgeOut = false;
	applyCartridge();
	cartridgeSaveUnread = cartridgeOut && !noGame;
	resetAdvance(gba);
	replayHint = !actionReplayLoaded ? HINT_NONE : !replayHasGame ? HINT_NO_GAME : fcdLoaded ? HINT_FCD : HINT_ADVANCE;
	if (actionReplayLoaded && *replayGamePath) {
		// The game on top keeps its own save, where it is when the game runs by itself; the
		// frontend keeps the Action Replay's flash.
		readSave(0, replayGamePath);
	} else if (!noGame) {
		readPlayerOneSave();
	}
	linkSetGba(gba);
	attachPorts();
	if (!noGame && (second || togetherWanted() || pairFile)) {
		startOthers(second, game->data, game->size);
	}
	offerLinkNetplay();
	showOptions();
	announceMemory();
	ereaderStart();
	struct retro_rumble_interface rumble = { NULL };
	setRumble = gba->gpio.devices & HW_RUMBLE && environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) ? rumble.set_rumble_state : NULL;
	rumbling = false;
	return true;
}

static bool loadPairFile(const struct retro_game_info* file) {
	char games[MAX_PLAYERS][PATH_SIZE];
	pairCount = pairFileGames(file->path, file->data, file->size, games);
	if (!pairCount) {
		showMessage("armGBA: a pair file needs two to four lines, one game each");
		return false;
	}
	struct retro_game_info first = { games[0], NULL, 0, NULL };
	void* data = readPatched(games[0], &first.size);
	first.data = data;
	if (!data) {
		char message[300];
		snprintf(message, sizeof(message), "armGBA can't read %.250s", games[0]);
		showMessage(message);
		return false;
	}
	// The other players' games, empty where player 1's game is named again.
	for (int i = 1; i < MAX_PLAYERS; ++i) {
		snprintf(i == 1 ? secondPath : otherPaths[i], PATH_SIZE, "%s", i < pairCount && strcmp(games[i], games[0]) ? games[i] : "");
	}
	pairFile = true;
	snprintf(pairPath, sizeof(pairPath), "%s", file->path);
	bool loaded = false;
	if (isGameBoyRom(data, first.size)) {
		gameBoy = false;
		loaded = loadGameBoy(&first, NULL);
	} else {
		loaded = loadAdvance(&first, NULL);
	}
	if (loaded) {
		readSave(0, firstPath);
	} else {
		pairFile = false;
	}
	free(data);
	return loaded;
}

// No game: a Game Boy Advance with no cartridge, for a single-pak game another console sends over
// the cable.
static bool loadNoGame(void) {
	static const struct retro_game_info none = { NULL, "", 0, NULL };
	noGame = true;
	gameBoy = false;
	return loadAdvance(&none, NULL);
}

bool retro_load_game(const struct retro_game_info* game) {
	readSaveActions(false);
	readReplayPress(false);
	controls.replayPressing = 0;
	stopFastForward();
	megaDuckGame = false;
	libretroGbSetMegaDuck(-1);
	libretroGbSetMegaDuckSlot(NULL, 0, 0);
	if (!game) {
		return loadNoGame();
	}
	if (!game->data && (!game->path || isPairFile(game->path) || hasGameBoyExtension(game->path))) {
		return false;
	}
	if (isPairFile(game->path)) {
		return loadPairFile(game);
	}
	gameBoy = false;
	megaDuckGame = isMegaDuck(game);
	libretroGbSetMegaDuck(megaDuckKind(game));
	libretroGbSetMegaDuckSlot(NULL, 0, 0);
	if (megaDuckGame) {
		offerSlotGames(game->path);
		const char* const slot = chosenSlot();
		if (readMegaDuckOptions(megaDuckKind(game), false) && slot) {
			size_t size = 0;
			void* data = gameRead(slot, &size);
			if (data) {
				struct retro_game_info slotGame = { slot, data, size, NULL };
				bool const loaded = loadMegaDuckLaptop(game, &slotGame);
				free(data);
				return loaded;
			}
		}
	}
	if (hasGameBoyExtension(game->path) || isMegaDuck(game)) {
		return loadGameBoy(game, NULL);
	}
	return loadAdvance(game, NULL);
}

// The laptop with a cartridge in its slot: the laptop's system ROM (or another MD0 program, its kind
// unless its extension says otherwise) runs, padded to the bigger of the two so the cartridge's ROM
// can take its place when the laptop runs it.
static bool loadMegaDuckLaptop(const struct retro_game_info* system, const struct retro_game_info* slot) {
	if (!system->size || !slot->size) {
		return false;
	}
	int systemKind = megaDuckKind(system);
	if (systemKind <= 0) {
		systemKind = 2;
	}
	int slotKind = megaDuckKind(slot);
	if (slotKind < 0) {
		slotKind = 0;
	}
	size_t room = 0x4000;
	while (room < system->size || room < slot->size) {
		room *= 2;
	}
	static uint8_t* padded;
	free(padded);
	padded = malloc(room);
	if (!padded) {
		return false;
	}
	for (size_t at = 0; at < room; at += system->size) {
		memcpy(padded + at, system->data, room - at < system->size ? room - at : system->size);
	}
	struct retro_game_info game = *system;
	game.data = padded;
	game.size = room;
	gameBoy = false;
	megaDuckGame = true;
	libretroGbSetMegaDuck(systemKind);
	readMegaDuckOptions(systemKind, true);
	libretroGbSetMegaDuckSlot(slot->data, slot->size, slotKind);
	return loadGameBoy(&game, NULL);
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
	readSaveActions(false);
	readReplayPress(false);
	controls.replayPressing = 0;
	stopFastForward();
	megaDuckGame = false;
	libretroGbSetMegaDuck(-1);
	if (type == SUBSYSTEM_ADVANCE_LINK && count == 2 && (info[0].data || info[0].path) && (info[1].data || info[1].path)) {
		gameBoy = false;
		return loadAdvance(&info[0], &info[1]);
	}
	if (type == SUBSYSTEM_LINK && count == 2 && info[0].data && info[1].data && hasGameBoyExtension(info[0].path) && hasGameBoyExtension(info[1].path)) {
		gameBoy = false;
		return loadGameBoy(&info[0], &info[1]);
	}
	if (type == SUBSYSTEM_MEGADUCK_LAPTOP && count == 2 && info[0].data && info[1].data && info[0].size && info[1].size) {
		return loadMegaDuckLaptop(&info[0], &info[1]);
	}
	if (type == SUBSYSTEM_ACTION_REPLAY && count == 2 && info[0].data && (info[1].data || info[1].path)) {
		// Only an Action Replay goes underneath; anything else isn't loaded at all.
		bool const gameBoyReplay = isGameBoyActionReplay(info[0].data, info[0].size) || isGameGenie(info[0].data, info[0].size)
		                           || isXploder(info[0].data, info[0].size);
		if (!gameBoyReplay && !actionReplayDetect(info[0].data, info[0].size)) {
			showMessage("armGBA: the Cheat Cartridge subsystem's first file isn't a cheat cartridge");
			return false;
		}
		gameBoy = false;
		replayGameGiven = &info[1];
		bool const loaded = gameBoyReplay ? loadGameBoy(&info[0], NULL) : loadAdvance(&info[0], NULL);
		replayGameGiven = NULL;
		return loaded && actionReplayLoaded;
	}
	return false;
}

void retro_unload_game(void) {
	useSensors(false, false);
	if (!secondFromFrontend) {
		rememberSecondChoice();
	}
	if (megaDuckGame) {
		rememberSlotChoice();
	}
	if (actionReplayLoaded) {
		rememberReplayChoice();
		keepReplayFlash(true);
	}
	keepReplayFactory(NULL, 0);
	stopFastForward();
	actionReplayLoaded = gameGenieLoaded = xploderLoaded = false;
	fcdLoaded = NULL;
	replayGamePath[0] = '\0';
	writeCoreSaves(true);
	forgetSaves();
	clockForget();
	ereaderForget();
	printoutFinish();
	largestWidth = largestHeight = 0;
	pairFile = false;
	secondFromFrontend = false;
	secondPath[0] = '\0';
	netplaySeen = false;
	importWaiting = false;
	netplayJoined = false;
	sessionSaveDue = false;
	clientTold = sessionOver = playingOn = clientIndexUnknown = false;
	noGame = false;
	sharedHinted = false;
	memset(&passed, 0, sizeof(passed));
	sessionTag = 0;
	netplayClient = -1;
	updateRumble(false);
	cheatsClear(&cheats, NULL);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
	if (gameBoy) {
		for (int i = playersHere - 1; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbUnload();
		}
		gameBoy = false;
		gbTogether = false;
		gbAdapter = false;
		libretroGbSetPlayers(1, false);
	}
	playersHere = 1;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		otherPaths[i][0] = '\0';
	}
	linkSetGba(NULL);
	dolphinSetHost(NULL);
	gamecubeShown = false;
	workersStop();
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (gbas[i]) {
			gbaDestroy(gbas[i]);
		}
		gbas[i] = NULL;
	}
	gba = NULL;
	gbaTogether = false;
}

void retro_reset(void) {
	// Restart can come straight after an option change, before a frame has read it, so read the
	// options first.
	readOptions();
	readClockOptions(true);
	memset(&passed, 0, sizeof(passed));
	applyLinkOptions();
	offerLinkNetplay();
	if (gameBoy) {
		booting = true;
		applyCpuClock(false);
		for (int i = playersHere - 1; i >= 0; --i) {
			libretroGbSelect(i);
			libretroGbReset(gbModel);
		}
		libretroGbSetPlayers(playersHere, gbAdapter);
		checkGameBoyMemory();
		return;
	}
	attachPorts();
	bool const putBack = applyCartridge();
	for (int i = 0; i < playersHere; ++i) {
		loadBios(gbas[i]);
		resetAdvance(gbas[i]);
	}
	if (putBack) {
		readCartridgeSave();
	}
	setAdvanceOffsets();
}

// Input.

// Turns the frontend's motion sensors on or off: on for Device Motion with a cartridge that has a
// tilt sensor or a gyro, outside netplay, where the other players couldn't see them.
static void useSensors(bool tilt, bool gyro) {
	bool const wanted = tiltControl == TILT_DEVICE && !netplaySeen;
	tilt &= wanted;
	gyro &= wanted;
	if ((tilt || gyro) && !sensors.set_sensor_state && !environment(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensors)) {
		memset(&sensors, 0, sizeof(sensors));
	}
	if (!sensors.set_sensor_state) {
		return;
	}
	if (tilt != accelerometerOn) {
		accelerometerOn = sensors.set_sensor_state(0, tilt ? RETRO_SENSOR_ACCELEROMETER_ENABLE : RETRO_SENSOR_ACCELEROMETER_DISABLE, 60) && tilt;
	}
	if (gyro != gyroscopeOn) {
		gyroscopeOn = sensors.set_sensor_state(0, gyro ? RETRO_SENSOR_GYROSCOPE_ENABLE : RETRO_SENSOR_GYROSCOPE_DISABLE, 60) && gyro;
	}
}

// Device Motion: gravity and the device's own movement along the screen's right and down, as the
// cartridge's sensor would feel them, 1 g giving 0x7FFF (a stick pushed all the way) and capped at
// 2 g so that a flick still counts. False with no readings, for the left stick to stand in.
static bool deviceMotion(int* x, int* y) {
	if (!accelerometerOn) {
		return false;
	}
	float const sensorX = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_X);
	float const sensorY = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Y);
	float const out = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Z);
	if (sensorX == 0 && sensorY == 0 && out == 0) {
		return false;
	}
	// The sensor's axes turned to the screen's, for a frontend that passes them on as the device has
	// them (RetroArch on a handheld whose screen is naturally upright, for one).
	static const float turns[4][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 } };
	const float* const turn = turns[motionRotation];
	float const right = turn[0] * sensorX + turn[1] * sensorY;
	float const up = turn[2] * sensorX + turn[3] * sensorY;
	// The sensor reads the push that holds the device up, so a side tipped down reads away from it.
	float const scale = 0x7FFF / 9.80665f;
	*x = (int) fmaxf(-0xFFFE, fminf(0xFFFE, -right * scale));
	*y = (int) fmaxf(-0xFFFE, fminf(0xFFFE, up * scale));
	return true;
}

// Reads the motion sensor control: a stick, or the D-pad pushed all the way, giving -0x7FFF to
// 0x7FFF on each axis, or the device's motion.
static void readMotion(uint16_t keys, int* x, int* y) {
	if (deviceMotion(x, y)) {
		return;
	}
	if (tiltControl == TILT_DPAD) {
		*x = keys & KEY_RIGHT ? 0x7FFF : keys & KEY_LEFT ? -0x7FFF : 0;
		*y = keys & KEY_DOWN ? 0x7FFF : keys & KEY_UP ? -0x7FFF : 0;
	} else {
		unsigned stick = tiltControl == TILT_RIGHT_STICK ? RETRO_DEVICE_INDEX_ANALOG_RIGHT : RETRO_DEVICE_INDEX_ANALOG_LEFT;
		*x = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X);
		*y = inputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y);
	}
}

// Feeds the tilt sensor of a Game Boy cartridge that has one (Kirby Tilt 'n' Tumble). L flicks the
// console upwards, which is how Kirby jumps, whatever tilts it (a Game Boy has no L); R still
// stretches the picture.
static void readTilt(uint16_t keys) {
	int x;
	int y;
	bool const click = keys & KEY_L;
	useSensors(true, false);
	readMotion(keys, &x, &y);
	if (click && !pressed.flickHeld) {
		libretroGbFlick();
	}
	pressed.flickHeld = click;
	libretroGbSetTilt(x, y);
}

// Whether pushing the right stick up or down turns the Game Boy's contrast wheel: on a screen that
// has one, unless the right stick tilts the cartridge.
static bool contrastStick(void) {
	return lcdCastsShadow() && !(tiltCartridge && tiltControl == TILT_RIGHT_STICK);
}

// Whether a stick click is the Action Replay's button.
static bool replayStick(unsigned button) {
	return replayHasButton() && button == replayButton;
}

// Open Action Replay Menu presses the Game Boy Action Replay's button once when chosen, then goes back
// to Off. Chosen when a game starts, as RetroArch may have kept it, it only goes back to Off. (The
// Game Boy Advance ones have no menu: their button codes want the button held.)
static void readReplayPress(bool act) {
	// Hold Action Replay Button starts each game Off: a game left with it held could start stopped.
	if (!act && optionIs("armgba_action_replay_hold", "on")) {
		struct retro_variable off = { "armgba_action_replay_hold", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		replayHoldOption = false;
		controls.replayHold = 0;
	}
	if (optionIs("armgba_action_replay_press", "press")) {
		struct retro_variable off = { "armgba_action_replay_press", "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
		if (act && actionReplayLoaded && gameBoy && !netplaySeen) {
			controls.replayPressing = REPLAY_PRESS_FRAMES;
		}
	}
}

// Whether the Action Replay's button is held this frame: by its stick click, by Hold Action Replay
// Button, or for Open Action Replay Menu's frames. On the Game Boy Advance, the stick click toggles
// Hold Action Replay Button by default, so a button code keeps working without the stick held in. What
// it keeps is in the state and goes on in every frame run, so rollback and run-ahead run it alike;
// the option and the notification change only with a frame shown.
static bool replayButtonHeld(void) {
	bool const pressing = controls.replayPressing > 0;
	if (pressing) {
		--controls.replayPressing;
	}
	// Only a cartridge with a button has a stick click for it (the Xploder Advance and CodeBreaker
	// have none)
	bool const click = replayButton != REPLAY_BUTTON_OFF && replayStick(replayButton) && inputState(0, RETRO_DEVICE_JOYPAD, 0, replayButton);
	bool const toggling = !gameBoy && replayToggles;
	if (toggling && click && !controls.replayClicked) {
		controls.replayHold = !controls.replayHold;
	}
	controls.replayClicked = click;
	// The option and the notification follow on a frame shown: with run-ahead, the click is first
	// seen in a frame run ahead, whose state the frame shown starts from.
	if (toggling && !rerunning() && controls.replayHold != replayHoldOption) {
		replayHoldOption = controls.replayHold;
		struct retro_variable hold = { "armgba_action_replay_hold", controls.replayHold ? "on" : "off" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &hold);
		char message[48];
		snprintf(message, sizeof(message), "%s button %s", cartridgeName(), controls.replayHold ? "held" : "released");
		notify(message, 120);
	}
	return pressing || (controls.replayHold && !gameBoy) || (!toggling && click);
}

// Whether clicking the right stick resets the contrast: R3 isn't a Restart Button or Switch Player
// Combo's, or the Action Replay's button.
static bool contrastClick(void) {
	return restartButtons != RESTART_STICKS && !switchStick(RETRO_DEVICE_ID_JOYPAD_R3) && !replayStick(RETRO_DEVICE_ID_JOYPAD_R3);
}

// Pushing the right stick up or down turns the contrast wheel a step, and clicking it goes back to 0;
// the option keeps its place.
static void readContrast(void) {
	static bool held, clicked;
	if (rerunning() || !contrastStick()) {
		return;
	}
	int const push = inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
	bool const up = push < -0x4000;
	bool const down = push > 0x4000;
	bool const click = contrastClick() && inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
	bool const stepped = (up || down) && !held;
	bool const reset = click && !clicked;
	held = up || down;
	clicked = click;
	if (!stepped && !reset) {
		return;
	}
	gbContrast = reset ? 0 : gbContrast + (up ? gbContrast < 5 : -(gbContrast > -5));
	for (int i = playersHere - 1; i >= 0; --i) {
		libretroGbSelect(i);
		libretroGbSetContrast(gbContrast);
	}
	char value[4];
	snprintf(value, sizeof(value), "%d", gbContrast);
	struct retro_variable variable = { "armgba_gb_contrast", value };
	environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
	char message[32];
	snprintf(message, sizeof(message), "Contrast %d%%", (gbContrast + 5) * 10);
	notify(message, 120);
}

// Feeds a Game Boy Advance cartridge's sensors before each frame. The tilt sensor and the gyro are
// scaled as mGBA's SDL frontend scales a stick; the gyro's turn comes from the control's left and
// right. The light sensor uses mGBA's levels, and pushing the right stick up or down changes the
// level by one step.
static void readSensors(uint16_t keys) {
	int devices = gba->gpio.devices;
	if (devices & (HW_TILT | HW_GYRO)) {
		int x;
		int y;
		useSensors(devices & HW_TILT, devices & HW_GYRO);
		readMotion(keys, &x, &y);
		gba->gpio.tiltInputX = x * 0x3800;
		gba->gpio.tiltInputY = y * 0x3800;
		gba->gpio.gyroZ = x * 11000;
		if (gyroscopeOn) {
			// The cartridge's gyro reads up to about 300 degrees a second (5 rad/s) either way;
			// turning clockwise as the stick's right does.
			float const turn = -sensors.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Z) / 5.0f;
			gba->gpio.gyroZ = (int32_t) (fmaxf(-2, fminf(2, turn)) * 0x7FFF * 11000);
		}
	}
	if (devices & HW_LIGHT_SENSOR) {
		static const int levels[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };
		int const push = inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		bool brighter = push < -0x4000;
		bool darker = push > 0x4000;
		if ((brighter || darker) && !pressed.solarHeld) {
			solarLevel += brighter ? (solarLevel < 10) : -(solarLevel > 0);
			char message[32];
			snprintf(message, sizeof(message), "Sunlight %d of 10", solarLevel);
			notify(message, 120);
		}
		pressed.solarHeld = brighter || darker;
		gba->gpio.luminance = 0xFF - (0x16 + (solarLevel ? levels[solarLevel - 1] : 0));
	}
}

// Returns whether a held turbo button presses its button this frame: pressed for the first frames
// of every turbo period, as in gambatte's core.
static bool turbo(unsigned port, unsigned button, uint8_t* counter) {
	if (!inputState(port, RETRO_DEVICE_JOYPAD, 0, button)) {
		*counter = 0;
		return false;
	}
	bool const down = *counter < turboPulse;
	*counter = (uint8_t) (*counter + 1u >= turboPeriod ? 0 : *counter + 1u);
	return down;
}

// Turbo L and R: with L2 and R2 set to them, L2 and R2 held press a Game Boy Advance game's L and R,
// except while Start holds them for the Switch Controller Combo.
static uint16_t readTurboShoulders(void) {
	if (!TRIGGERS_TURBO) {
		return 0;
	}
	bool const l = !switchStart(0) && turbo(0, RETRO_DEVICE_ID_JOYPAD_L2, &pressed.turboL);
	bool const r = !switchStart(0) && turbo(0, RETRO_DEVICE_ID_JOYPAD_R2, &pressed.turboR);
	return (l ? KEY_L : 0) | (r ? KEY_R : 0);
}

// Whether L3 is free for L3 Button: not off, and not the Action Replay's button or in the Restart or
// Switch Controller Combo.
static bool l3Free(void) {
	return l3Button != L3_OFF && !replayStick(RETRO_DEVICE_ID_JOYPAD_L3) && !switchStick(RETRO_DEVICE_ID_JOYPAD_L3) &&
	       restartButtons != RESTART_STICKS;
}

// L3 Button: its click turns Fast-Forward Toggle's fast-forward on or off, or held for half a
// second, Soft Reset presses A + B + Select + Start for a few frames, which most games take as a
// reset; held on, it doesn't again until let go. What it keeps is in the state and goes on in every
// frame run, as the Action Replay's button's does; fast-forward, which isn't the game's, changes only
// with a frame shown (and never in netplay).
static void readL3Button(void) {
	bool const now = l3Free() && inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
	// Fast-forward is the frontend's, so it toggles on the frames shown, from whether L3 was held on
	// the last one shown: with run-ahead, the click is first seen in a frame run ahead.
	static bool heldShown;
	// The latch is L2 and R2's toggle's too, so it stays while either button toggles
	if ((l3Button != L3_FAST_FORWARD && !triggersToggle) || linkSession() || netplaySeen) {
		fastForwardLatched = false;
	} else if (l3Button == L3_FAST_FORWARD && now && !heldShown && !rerunning()) {
		fastForwardLatched = !fastForwardLatched;
	}
	if (!rerunning()) {
		heldShown = now;
	}
	if (controls.softResetFrames) {
		--controls.softResetFrames;
	}
	controls.l3HeldFor = now && l3Button == L3_SOFT_RESET ? (uint8_t) (controls.l3HeldFor < SOFT_RESET_HOLD ? controls.l3HeldFor + 1 : SOFT_RESET_HOLD + 1) : 0;
	if (controls.l3HeldFor == SOFT_RESET_HOLD) {
		controls.softResetFrames = SOFT_RESET_PRESS;
	}
	controls.l3Held = now;
}

static bool allHeld(uint16_t keys, uint16_t wanted) {
	return (keys & wanted) == wanted;
}

// As in mGBA's core by default, pressing opposite directions together presses neither.
static uint16_t withoutOpposites(uint16_t keys) {
	if (allHeld(keys, KEY_LEFT | KEY_RIGHT)) {
		keys &= ~(KEY_LEFT | KEY_RIGHT);
	}
	if (allHeld(keys, KEY_UP | KEY_DOWN)) {
		keys &= ~(KEY_UP | KEY_DOWN);
	}
	return keys;
}

// Returns the buttons held on a controller, as KEY_ bits.
static uint16_t readKeys(unsigned port) {
	static const unsigned buttons[10] = {
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_SELECT,
		RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_RIGHT,
		RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_UP,
		RETRO_DEVICE_ID_JOYPAD_DOWN,
		RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_L,
	};
	uint16_t keys = 0;
	for (int i = 0; i < 10; ++i) {
		if (inputState(port, RETRO_DEVICE_JOYPAD, 0, buttons[i])) {
			keys |= 1 << i;
		}
	}
	// Turbo A and B: X and Y held press A and B.
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_X, &pressed.turboA[port]) ? KEY_A : 0;
	keys |= turbo(port, RETRO_DEVICE_ID_JOYPAD_Y, &pressed.turboB[port]) ? KEY_B : 0;
	// The left stick as the D-pad, unless it tilts controller 1's motion sensor cartridge.
	bool const tilting = port == 0 && tiltControl == TILT_LEFT_STICK &&
	                     (gameBoy ? tiltCartridge : gba && gba->gpio.devices & (HW_TILT | HW_GYRO));
	if (leftStickDpad && !tilting) {
		int const x = inputState(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		int const y = inputState(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		keys |= (x < -0x4000 ? KEY_LEFT : 0) | (x > 0x4000 ? KEY_RIGHT : 0) | (y < -0x4000 ? KEY_UP : 0) | (y > 0x4000 ? KEY_DOWN : 0);
	}
	// Start + L3 or R3 switches controllers: with the stick clicked first, the game doesn't see Start.
	unsigned const stick = switchController == SWITCH_START_L3 ? RETRO_DEVICE_ID_JOYPAD_L3 : RETRO_DEVICE_ID_JOYPAD_R3;
	if (port == 0 && switchController >= SWITCH_START_L3 && switchStick(stick) && inputState(0, RETRO_DEVICE_JOYPAD, 0, stick)) {
		keys &= ~KEY_START;
	}
	if (port == 0 && controls.softResetFrames) {
		keys |= KEY_A | KEY_B | KEY_SELECT | KEY_START;
	}
	return withoutOpposites(keys);
}

// On the Game Boy Advance model, L or R switches between the game's own size and stretched across
// the screen, as that console's shoulder buttons do; with a tilt cartridge only R, as L flicks.
static void readStretch(uint16_t keys) {
	static bool held;
	bool now = keys & (tiltCartridge ? KEY_R : KEY_L | KEY_R);
	if (now && !held) {
		gbStretched = !gbStretched;
		if (gbCropped) {
			announceGeometry();
		}
	}
	held = now;
}

// Frame Skip: 0 off, -1 Auto, else how many frames are left out after each one shown; for Auto,
// how full the frontend's sound buffer is, as it reports it.
static int frameSkip;
static bool soundActive, soundShort;
static unsigned soundFull;
bool frameSkipped;
int framesSkipped;
// How many frames have been left out, for a frontend's numbers.
unsigned long frameSkipTotal;

static void soundBufferStatus(bool active, unsigned occupancy, bool underrunLikely) {
	soundActive = active;
	soundFull = occupancy;
	soundShort = underrunLikely;
}

// Fast-Forward Skip: frames the frontend won't show while it fast-forwards aren't drawn.
static bool fastForwardSkip;

static void readFrameSkip(void) {
	static const struct Choice choices[] = { { "auto", -1 }, { "1", 1 }, { "2", 2 }, { "3", 3 } };
	frameSkip = optionChoice("armgba_frame_skip", choices, COUNT(choices), 0);
	fastForwardSkip = !optionIs("armgba_fast_forward_skip", "disabled");
	struct retro_audio_buffer_status_callback status = { frameSkip < 0 ? soundBufferStatus : NULL };
	if (!environment(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &status) || frameSkip >= 0) {
		soundActive = false;
	}
}

// Whether this frame is left out: with Auto while the sound is about to run short, a third full or
// less, and no more than three in a row; otherwise the set count after each one shown. Never in
// netplay, where every device's state must match, nor for a frame RetroArch runs again. With
// Fast-Forward Skip, a GBA frame the frontend won't show while it fast-forwards is left out too.
static bool skipThisFrame(void) {
	static int run;
	bool skip = false;
	if (frameSkip && !netplaySeen && !rerunning()) {
		skip = frameSkip < 0 ? soundActive && (soundShort || soundFull <= 33) && run < 3 : run < frameSkip;
	}
	bool fastForwarding = false;
	if (!skip && fastForwardSkip && !gameBoy && !netplaySeen && rerunning() &&
	    environment(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fastForwarding) && fastForwarding) {
		skip = true;
	}
	if (!skip) {
		framesSkipped = run;
		run = 0;
	} else {
		++run;
		++frameSkipTotal;
	}
	return skip;
}

// Whether RetroArch is running this frame again, for a netplay rollback or runahead, rather than
// for the first time.
bool rerunning(void) {
	int enabled = RETRO_AV_ENABLE_VIDEO;
	return environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &enabled) && !(enabled & RETRO_AV_ENABLE_VIDEO);
}

// Whether Switch Controller Combo works now: on, with the consoles here and not in netplay.
static bool switchOn(void) {
	return switchController != SWITCH_OFF && togetherRunning() && (!netplaySeen || playingOn) && playersHere > 1;
}

// Each Switch Controller Combo's buttons, and the game's buttons among them.
static const struct {
	size_t count;
	unsigned buttons[4];
	uint16_t keys;
} combos[] = {
	[SWITCH_L_R_A_B] = { 4, { RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B }, KEY_L | KEY_R | KEY_A | KEY_B },
	[SWITCH_L3_R3] = { 2, { RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3 }, 0 },
	[SWITCH_START_L2_R2] = { 3, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2 }, KEY_START },
	[SWITCH_START_L3] = { 2, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L3 }, KEY_START },
	[SWITCH_START_R3] = { 2, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_R3 }, KEY_START },
};

// Whether a controller holds a combo's buttons.
static bool comboHeld(int combo, int port) {
	for (size_t k = 0; k < combos[combo].count; ++k) {
		if (!inputState(port, RETRO_DEVICE_JOYPAD, 0, combos[combo].buttons[k])) {
			return false;
		}
	}
	return true;
}

// Whether controller 1 holds the Switch Controller Combo buttons.
static bool switchHeld(int port) {
	return port == 0 && switchOn() && comboHeld(switchController, 0);
}

// Whether controller 1's L3 or R3 is one of the Switch Controller Combo buttons now, so it only
// switches.
static bool switchStick(unsigned button) {
	return switchOn() && (switchController == SWITCH_L3_R3 ||
	                      (switchController == SWITCH_START_L3 && button == RETRO_DEVICE_ID_JOYPAD_L3) ||
	                      (switchController == SWITCH_START_R3 && button == RETRO_DEVICE_ID_JOYPAD_R3));
}

// The controller that plays console i: controller 1 plays controllerFocus's.
static int portFor(int i) {
	if ((netplaySeen && !playingOn) || controllerFocus >= playersHere) {
		controllerFocus = 0;
	}
	return (i - controllerFocus + playersHere) % playersHere;
}

// Whether controller 1 holds Start with Switch Controller Combo on Start + L2 + R2, so its L2 and
// R2 only switch.
static bool switchStart(int port) {
	return switchController == SWITCH_START_L2_R2 && port == 0 && switchOn() &&
	       inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
}

// The Switch Controller Combo buttons on controller 1 move it to the next console.
static void readControllerSwitch(void) {
	static bool held;
	if (rerunning()) {
		return;
	}
	bool const now = switchHeld(0);
	bool const pressed = now && !held;
	held = now;
	if (!pressed) {
		return;
	}
	controllerFocus = (controllerFocus + 1) % playersHere;
	char name[48];
	snprintf(name, sizeof(name), "Controller 1: Player %d's Console", controllerFocus + 1);
	notify(name, 120);
}

// Shared Controls with one console: every player's buttons, or Pass Around's holder's, another
// player taking over with the Switch Controller Combo (L + R + A + B when it's off); the one who
// takes over isn't seen pressing the combo's buttons until letting go of them. Turbo L and R come
// from controller 1, as without it.
static uint16_t readSharedKeys(bool shoulders) {
	uint16_t keys[MAX_PLAYERS], everyone = 0;
	for (int port = 0; port < MAX_PLAYERS; ++port) {
		keys[port] = readKeys((unsigned) port) | (port == 0 && shoulders ? readTurboShoulders() : 0);
		everyone |= keys[port];
	}
	if (sharedControls == SHARED_EVERYONE) {
		return withoutOpposites(everyone);
	}
	int const combo = switchController == SWITCH_OFF ? SWITCH_L_R_A_B : switchController;
	for (int port = 0; port < MAX_PLAYERS; ++port) {
		bool const held = comboHeld(combo, port), was = passed.comboHeld >> port & 1;
		passed.comboHeld = (uint8_t) (held ? passed.comboHeld | 1 << port : passed.comboHeld & ~(1 << port));
		if (held && !was && port != passed.holder) {
			passed.holder = (uint8_t) port;
			passed.taking = 1;
			if (!rerunning()) {
				char message[40];
				snprintf(message, sizeof(message), "Player %d has the controls", port + 1);
				notify(message, 120);
			}
		}
	}
	uint16_t const own = keys[passed.holder % MAX_PLAYERS];
	passed.taking = passed.taking && (own & combos[combo].keys);
	return passed.taking ? own & ~combos[combo].keys : own;
}

// With Link Mode and Shared Controls off, only player 1 plays: the first time another player
// presses a button player 1 isn't pressing, says how to share the console. A second copy of
// controller 1, as some handhelds' input services leave, presses only what player 1 does. Buttons
// are read directly, as reading them as keys would step their turbo.
static void hintSharedControls(void) {
	if (!oneConsole || sharedControls != SHARED_OFF || sharedHinted || rerunning()) {
		return;
	}
	for (unsigned port = 1; port < MAX_PLAYERS; ++port) {
		for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R; ++id) {
			if (inputState(port, RETRO_DEVICE_JOYPAD, 0, id) && !inputState(0, RETRO_DEVICE_JOYPAD, 0, id)) {
				char message[128];
				snprintf(message, sizeof(message), "Player %u's buttons aren't used. To share this console, set Link > Shared Controls.", port + 1);
				notify(message, 300);
				sharedHinted = true;
				return;
			}
		}
	}
}

// L2 or R2 held fast-forwards at RetroArch's maximum run speed; with the consoles here only L2, as
// the L2 Button option says, or not with L2 and R2 set to Turbo L and R; or by L3's Fast-Forward
// Toggle. Not in netplay, where the other consoles would wait.
static bool fastForwardHeld;

static void setFastForward(bool on) {
	fastForwardHeld = on;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = on, .notification = true, .inhibit_toggle = on };
	environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

// A game closing or starting: fast-forward and L3's soft reset stop, so the next game starts at
// normal speed.
static void stopFastForward(void) {
	fastForwardLatched = false;
	memset(&controls, 0, sizeof(controls));
	if (fastForwardHeld) {
		setFastForward(false);
	}
}

static void readFastForward(void) {
	bool now = false;
	for (int port = 0; port < (togetherRunning() ? playersHere : 1); ++port) {
		now |= (!togetherRunning() || l2FastForward) && !switchStart(port) && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
		now |= !togetherRunning() && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
	}
	now &= !TRIGGERS_TURBO;
	// Fast-Forward Toggle: a press turns the latch L3's toggle shares on or off, on the frames shown
	// (with run-ahead, a press is first seen in a frame run ahead), from whether they were held on the
	// last one shown. L3's readL3Button clears it where neither toggles.
	static bool heldShown;
	if (triggersToggle && now && !heldShown && !rerunning() && !linkSession() && !netplaySeen) {
		fastForwardLatched = !fastForwardLatched;
	}
	if (!rerunning()) {
		heldShown = now;
	}
	now = (now && !triggersToggle) || fastForwardLatched;
	now &= !linkSession() && !netplaySeen;
	if (now != fastForwardHeld) {
		setFastForward(now);
	}
}

// Holding the Restart Combo for a second restarts that player's console, as its power switch
// would. Games that reset on A, B, Select and Start do so at once by themselves.
enum { RESTART_FRAMES = 60 };
static uint8_t restartHeld[MAX_PLAYERS];

void restartConsole(int player) {
	if (gameBoy) {
		booting = true;
		applyCpuClock(false);
		libretroGbSelect(player);
		libretroGbRestart();
		libretroGbSelect(0);
	} else {
		resetAdvance(gbas[player]);
	}
	char message[48];
	snprintf(message, sizeof(message), playersHere > 1 ? "Player %d's console restarted" : "Restarted", player + 1);
	notify(message, 120);
}

// Whether a player holds the Restart Combo.
static bool restartHeldDown(int port, uint16_t keys) {
	switch (restartButtons) {
	case RESTART_L_R_START:
		return allHeld(keys, KEY_L | KEY_R | KEY_START);
	case RESTART_L_R_A_B:
		return allHeld(keys, KEY_L | KEY_R | KEY_A | KEY_B);
	case RESTART_STICKS:
		return inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) && inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
	case RESTART_SELECT_START_A_B:
		return allHeld(keys, KEY_SELECT | KEY_START | KEY_A | KEY_B);
	default:
		return false;
	}
}

static void readRestarts(const uint16_t keys[]) {
	for (int i = 0; i < playersHere; ++i) {
		if (!restartHeldDown(portFor(i), keys[i]) || switchHeld(portFor(i))) {
			restartHeld[i] = 0;
		} else if (restartHeld[i] < RESTART_FRAMES && ++restartHeld[i] == RESTART_FRAMES) {
			restartConsole(i);
		}
	}
}


void libretroSetNetplayPlayer(int player) {
	netplayPlayer = player;
	playingOn &= player < 0;
}

void libretroPlayOn(void) {
	int const player = joinedPlayer();
	playingOn = true;
	controllerFocus = player < playersHere ? player : 0;
}

// The way of showing the screens step ways on from now, wrapping round.
static int nextScreens(int now, int step) {
	int ways[SCREENS_COUNT];
	int count = 0;
	ways[count++] = SCREENS_SIDE;
	if (playersHere == 2) {
		ways[count++] = SCREENS_STACKED;
	}
	for (int i = 0; i < playersHere; ++i) {
		ways[count++] = SCREENS_ONE + i;
	}
	int at = 0;
	for (int i = 0; i < count; ++i) {
		at = ways[i] == now ? i : at;
	}
	return ways[((at + step) % count + count) % count];
}

// R2 moves to the next way of showing the screens, and L2 back unless it fast-forwards. In netplay
// each device shows the screens its own way, changed by one player's L2 and R2: Screen Switch
// Controller's player, or with Auto this device's own, which the standalone frontend says, and
// RetroArch after 1.22.2; with one that doesn't, the host's player 1, or player 2 on a device that
// joined.
static void readScreensKeys(void) {
	static bool held[MAX_PLAYERS][2];
	if (rerunning() || TRIGGERS_TURBO) {
		return;
	}
	bool const netplay = (netplaySeen && !playingOn) || netplayPlayer >= 0;
	if (netplaySeen) {
		unsigned client = 0;
		bool const told = askClientIndex(&client) && client < MAX_PLAYERS;
		netplayClient = told && !(client == 0 && netplayJoined) ? (int) client : -1;
	}
	int here = netplayJoined ? 1 : 0;
	if (r2Follows >= 0) {
		here = r2Follows;
	} else if (netplayPlayer >= 0) {
		here = netplayPlayer;
	} else if (netplayClient >= 0 && netplayClient < playersHere) {
		here = netplayClient;
	}
	int step = 0;
	for (int port = 0; port < playersHere; ++port) {
		if (netplay && port != here) {
			continue;
		}
		for (int k = netplay || !l2FastForward ? 0 : 1; k < 2; ++k) {
			bool const now = !switchStart(port) && inputState(port, RETRO_DEVICE_JOYPAD, 0, k ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_L2);
			if (now && !held[port][k]) {
				step += k ? 1 : -1;
			}
			held[port][k] = now;
		}
	}
	if (!step) {
		return;
	}
	screens = nextScreens(screens, step);
	announceGeometry();
	char name[32];
	if (screens >= SCREENS_ONE) {
		snprintf(name, sizeof(name), "Player %d Only", screens - SCREENS_ONE + 1);
	} else {
		snprintf(name, sizeof(name), "%s", playersHere > 2 ? "Grid" : screens == SCREENS_SIDE ? "Side by Side" : "Top and Bottom");
	}
	notify(name, 120);
}

// Running a frame.

// The low-pass filter: each sample out, per side, is the last one out weighted by lowPass, out of
// 0x10000, and the new one by the rest.
static void sendSound(const int16_t* samples, size_t frames) {
	static int16_t filtered[2 * 4096];
	static int32_t last[2];
	if (megaDuckGame && !rerunning()) {
		samples = libretroGbMixSpeech(samples, frames);
	}
	if (!lowPass || rerunning()) {
		audioBatch(samples, frames);
		return;
	}
	while (frames) {
		size_t const n = frames < COUNT(filtered) / 2 ? frames : COUNT(filtered) / 2;
		for (size_t k = 0; k < n * 2; ++k) {
			last[k & 1] = (last[k & 1] * lowPass + samples[k] * (0x10000 - lowPass)) >> 16;
			filtered[k] = (int16_t) last[k & 1];
		}
		audioBatch(filtered, n);
		samples += n * 2;
		frames -= n;
	}
}

// Returns the picture as a Game Boy Advance shows it, on black: centered on its 240x160 screen, or
// stretched to 240x144 by doubling every other column.
static const uint32_t* advanceFrame(const uint32_t* picture) {
	uint32_t* row = gbAdvanceFrame + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH;
	int const border = (SCREEN_WIDTH - LIBRETRO_GB_WIDTH) / 2;
	for (int y = 0; y < LIBRETRO_GB_HEIGHT; ++y, row += SCREEN_WIDTH, picture += LIBRETRO_GB_WIDTH) {
		if (gbStretched) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				row[x] = picture[x * 2 / 3];
			}
		} else {
			memset(row, 0, border * sizeof(*row));
			memcpy(row + border, picture, LIBRETRO_GB_WIDTH * sizeof(*row));
			memset(row + border + LIBRETRO_GB_WIDTH, 0, border * sizeof(*row));
		}
	}
	return gbAdvanceFrame;
}

// Show Player Numbers: a badge at a screen's top left corner, its pitch bytes a row: "P" and the
// player's number in a 5 by 7 dot font, white on the player's color (blue, red, green, amber), with a
// darker edge and its corners rounded, faint over the picture.
static uint32_t blendDot(uint32_t picture, uint32_t color, int alpha, size_t pixelBytes) {
	unsigned r, g, b;
	if (pixelBytes == 4) {
		r = picture >> 16 & 0xFF, g = picture >> 8 & 0xFF, b = picture & 0xFF;
	} else {
		r = (picture >> 11) << 3, g = (picture >> 5 & 0x3F) << 2, b = (picture & 0x1F) << 3;
	}
	r += ((color >> 16 & 0xFF) - (int) r) * alpha / 256;
	g += ((color >> 8 & 0xFF) - (int) g) * alpha / 256;
	b += ((color & 0xFF) - (int) b) * alpha / 256;
	return pixelBytes == 4 ? r << 16 | g << 8 | b : (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
}

static void numberScreen(uint8_t* corner, size_t pitch, size_t pixelBytes, int player) {
	static const uint8_t glyphs[5][7] = {
		{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
		{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
	};
	static const uint32_t colors[MAX_PLAYERS] = { 0x2F6FE0, 0xD83C3C, 0x2E9E4A, 0xD99A12 };
	uint32_t const fill = colors[player], edge = (fill >> 1) & 0x7F7F7F;
	int const width = 15, height = 11;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			bool const cornerDot = (x == 0 || x == width - 1) && (y == 0 || y == height - 1);
			if (cornerDot) {
				continue;
			}
			bool const border = x == 0 || y == 0 || x == width - 1 || y == height - 1;
			int const gx = x - 2, gy = y - 2, glyph = gx < 5 ? 0 : 1, column = glyph ? gx - 6 : gx;
			bool const dot = !border && gy >= 0 && gy < 7 && column >= 0 && column < 5 && (gx < 5 || gx > 5) &&
			                 glyphs[glyph ? player + 1 : 0][gy] >> (4 - column) & 1;
			uint8_t* const at = corner + (y + 2) * pitch + (x + 2) * pixelBytes;
			uint32_t const was = pixelBytes == 4 ? *(uint32_t*) at : *(uint16_t*) at;
			uint32_t const now = dot ? blendDot(was, 0xFFFFFF, 170, pixelBytes) : blendDot(was, border ? edge : fill, border ? 110 : 90, pixelBytes);
			if (pixelBytes == 4) {
				*(uint32_t*) at = now;
			} else {
				*(uint16_t*) at = (uint16_t) now;
			}
		}
	}
}

// Puts the consoles' pictures side by side, one above the other, in a grid, or one alone, as chosen,
// and hands over their sound mixed, or the shown one's.
static void showConsoles(const void* const pictures[], size_t pixelBytes, int width, int height, void* frame, const int16_t* const samples[],
                         const size_t frames[]) {
	if (screens >= SCREENS_ONE) {
		int const shown = screens - SCREENS_ONE < playersHere ? screens - SCREENS_ONE : 0;
		if (playerNumbers) {
			memcpy(frame, pictures[shown], width * pixelBytes * height);
			numberScreen(frame, width * pixelBytes, pixelBytes, shown);
			showPicture(frame, width, height, width * pixelBytes);
		} else {
			showPicture(pictures[shown], width, height, width * pixelBytes);
		}
		sendSound(samples[shown], frames[shown]);
		return;
	}
	int const columns = playersHere == 2 && screens == SCREENS_STACKED ? 1 : 2;
	int const rows = (playersHere + columns - 1) / columns;
	size_t const row = width * pixelBytes;
	uint8_t* const out = frame;
	memset(out, 0, row * columns * height * rows);
	for (int i = 0; i < playersHere; ++i) {
		uint8_t* const corner = out + (i / columns) * height * row * columns + (i % columns) * row;
		for (int y = 0; y < height; ++y) {
			memcpy(corner + y * row * columns, (const uint8_t*) pictures[i] + y * row, row);
		}
		if (playerNumbers) {
			numberScreen(corner, row * columns, pixelBytes, i);
		}
	}
	showPicture(frame, width * columns, height * rows, row * columns);
	size_t count = COUNT(mixedSamples) / 2;
	for (int i = 0; i < playersHere; ++i) {
		count = frames[i] < count ? frames[i] : count;
	}
	for (size_t k = 0; k < count * 2; ++k) {
		int sum = 0;
		for (int i = 0; i < playersHere; ++i) {
			sum += samples[i][k];
		}
		mixedSamples[k] = sum / playersHere;
	}
	sendSound(mixedSamples, count);
}

static void runGameBoys(void) {
	uint16_t keys[MAX_PLAYERS];
	readControllerSwitch();
	for (int i = 0; i < playersHere; ++i) {
		keys[i] = readKeys(portFor(i));
	}
	readTilt(keys[0]);
	readContrast();
	readScreensKeys();
	readRestarts(keys);
	if (copySaves) {
		copyFirstSave();
	}
	const uint32_t* pictures[MAX_PLAYERS];
	const int16_t* samples[MAX_PLAYERS];
	size_t frames[MAX_PLAYERS];
	libretroGbRunPlayers(keys, pictures, samples, frames);
	const void* shown[MAX_PLAYERS];
	for (int i = 0; i < playersHere; ++i) {
		shown[i] = pictures[i];
	}
	showConsoles(shown, sizeof(uint32_t), LIBRETRO_GB_WIDTH, LIBRETRO_GB_HEIGHT, gbTogetherFrame, samples, frames);
}

static void airSend(const struct RfuAir* air, int to, const uint8_t* message, size_t size) {
	int const from = air->self;
	if (airQueued[from] == AIR_QUEUE || size > AIR_BYTES) {
		return;
	}
	airQueue[from][airQueued[from]].to = to;
	airQueue[from][airQueued[from]].size = size;
	memcpy(airQueue[from][airQueued[from]].data, message, size);
	++airQueued[from];
}

// Between rounds: each console's messages reach the others, in the order the consoles come, and the
// answers they bring go out the same way.
static void carryAir(void) {
	for (bool any = true; any;) {
		any = false;
		for (int from = 0; from < MAX_PLAYERS; ++from) {
			int const count = airQueued[from];
			airQueued[from] = 0;
			for (int k = 0; k < count; ++k) {
				any = true;
				for (int i = 0; i < MAX_PLAYERS; ++i) {
					if (i != from && gbas[i] && (airQueue[from][k].to < 0 || airQueue[from][k].to == i)) {
						rfuReceive(gbas[i], from, airQueue[from][k].data, airQueue[from][k].size);
					}
				}
			}
		}
	}
}

// The cable between the Game Boy Advances here. When a transfer ends:
// - in normal mode, players 1 and 2 swap the data each had as it started; the console that didn't
//   start it takes the data only if it is waiting for a transfer. With three or four, the parent's
//   data shifts along the cable instead (see exchange);
// - in multiplayer mode, the parent's word and each child's go to every child.

// Where a console is in the frame while it runs in a round.
static int32_t roundNow(int i) {
	return roundDone[i] + (int32_t) ((uint32_t) timingCurrentTime(gbas[i]) - roundStartTime[i]);
}

static uint32_t dataOf(int i) {
	int mode;
	bool ready;
	return sioOutgoing(gbas[i], &mode, &ready);
}

static void cableBegin(void* context, int mode, uint32_t out, int32_t cycles) {
	int const i = (int) (intptr_t) context;
	struct Transfer* const t = &transfers[i];
	memset(t, 0, sizeof(*t));
	t->mode = mode;
	t->out = out;
	int32_t const start = roundNow(i);
	for (int k = 0; k < playersHere; ++k) {
		if (k != i) {
			t->takeAt[k] = start + (mode == MULTIPLAYER ? cycles : 0);
			t->pending |= 1 << k;
		}
	}
}

static void cableWrote(void* context) {
	int const i = (int) (intptr_t) context;
	int const n = dataWrites[i].count;
	if (n < DATA_WRITES) {
		dataWrites[i].at[n] = roundNow(i);
		dataWrites[i].value[n] = dataOf(i);
		dataWrites[i].count = n + 1;
	}
}

// A console's data at a moment of the round just run.
static uint32_t dataAt(int i, int32_t when) {
	uint32_t value = dataWrites[i].before;
	for (int k = 0; k < dataWrites[i].count && dataWrites[i].at[k] <= when; ++k) {
		value = dataWrites[i].value[k];
	}
	return value;
}

// Between rounds: the data due in the round, for each transfer under way, or a waiting console's,
// which can't change; then the writes are forgotten, and each console's data is noted as the next
// round begins.
static void takeData(void) {
	for (int i = 0; i < playersHere; ++i) {
		struct Transfer* const t = &transfers[i];
		for (int k = 0; t->pending && k < playersHere; ++k) {
			if ((t->pending & 1 << k) && (t->takeAt[k] <= roundDone[k] || transfers[k].waiting)) {
				t->data[k] = dataAt(k, t->takeAt[k]);
				t->pending &= ~(1 << k);
			}
		}
	}
	for (int i = 0; i < playersHere; ++i) {
		dataWrites[i].count = 0;
		dataWrites[i].before = dataOf(i);
	}
}

static bool cableFinish(void* context, uint32_t in[3]) {
	int const i = (int) (intptr_t) context;
	struct Transfer* const t = &transfers[i];
	if (!t->answered) {
		t->waiting = true;
		gbas[i]->returnRequested = true;
		timingInterrupt(gbas[i]);
		return false;
	}
	t->answered = false;
	memcpy(in, t->in, sizeof(t->in));
	return true;
}

// Between rounds: the children see a multiplayer transfer the parent has under way.
static void carryBusy(void) {
	for (int i = 0; i < playersHere; ++i) {
		if (transfers[i].mode != MULTIPLAYER || sioTransferLeft(gbas[i]) <= 0) {
			continue;
		}
		for (int k = 0; k < playersHere; ++k) {
			if (k != i) {
				sioOthersTransfer(gbas[k]);
			}
		}
	}
}

// Between rounds: each console's SD line is high while every other console leaves it high.
static void carryReady(void) {
	for (int i = 0; i < playersHere; ++i) {
		bool ready = true;
		for (int k = 0; k < playersHere; ++k) {
			ready &= k == i || sioSdHigh(gbas[k]);
		}
		if (ready != cables[i].ready) {
			cables[i].ready = ready;
			sioLinkChanged(gbas[i]);
		}
	}
}

// A console's transfer ending: it takes the others' data, and they take its.
static void exchange(int i) {
	struct Transfer* const t = &transfers[i];
	t->answered = true;
	t->in[0] = 0xFFFFFFFF;
	t->in[1] = t->in[2] = 0xFFFF;
	int mode;
	bool ready;
	if (t->mode == MULTIPLAYER) {
		uint32_t values[4] = { t->out & 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
		for (int k = 1; k < playersHere; ++k) {
			sioOutgoing(gbas[k], &mode, &ready);
			values[k] = t->in[k - 1] = mode == MULTIPLAYER ? t->data[k] & 0xFFFF : 0xFFFF;
		}
		for (int k = 1; k < playersHere; ++k) {
			sioReceive(gbas[k], t->mode, values);
		}
	} else if (i == 0 && playersHere > 2) {
		// Normal mode with three or four on the cable: the data shifts along from the parent, each
		// console taking the one before's as the transfer started, and the parent takes zero.
		t->in[0] = 0;
		for (int k = 1; k < playersHere; ++k) {
			uint32_t const values[4] = { k == 1 ? t->out : t->data[k - 1], 0xFFFF, 0xFFFF, 0xFFFF };
			sioReceive(gbas[k], t->mode, values);
		}
	} else if (i < 2) {
		struct GBA* const other = gbas[1 - i];
		sioOutgoing(other, &mode, &ready);
		t->in[0] = ready && mode != MULTIPLAYER ? t->data[1 - i] : 0xFFFFFFFF;
		uint32_t const values[4] = { t->out, 0xFFFF, 0xFFFF, 0xFFFF };
		sioReceive(other, t->mode, values);
	}
}

// Between rounds: the transfers ending on this cycle, answered before they end; and any that ended
// inside a round, which waited for it to end. Consoles stop a few cycles past a round's end, so a
// transfer can end before another console has got to where its data is taken: it waits for it.
static void carryCable(void) {
	for (int i = 0; i < playersHere; ++i) {
		if (transfers[i].pending) {
			continue;
		}
		if (transfers[i].waiting || (!transfers[i].answered && sioTransferLeft(gbas[i]) == 0)) {
			transfers[i].waiting = false;
			exchange(i);
		}
	}
}

// The end of a console's part of a round, before anything else due on that cycle, such as a
// transfer ending.
static void sliceEnd(struct GBA* machine, void* context, uint32_t late) {
	(void) late;
	sliceOver[(intptr_t) context] = true;
	machine->returnRequested = true;
	timingInterrupt(machine);
}

// One console's part of a round: it runs to the round's end, or until a transfer it clocked waits for
// the others' data. A frame it finishes on the way is kept as its picture.
static int32_t roundEnd;

static void runRound(int i) {
	struct GBA* const machine = gbas[i];
	int32_t const left = roundEnd - roundDone[i];
	if (left <= 0 || transfers[i].waiting) {
		return;
	}
	uint32_t const start = timingCurrentTime(machine);
	roundStartTime[i] = start;
	sliceOver[i] = false;
	timingSchedule(machine, &sliceEvents[i], left);
	while (!sliceOver[i] && !transfers[i].waiting) {
		uint32_t const frame = machine->video.frameCounter;
		gbaRunPart(machine);
		if (machine->video.frameCounter != frame) {
			memcpy(gbaShown[i], machine->renderer.frame, sizeof(gbaShown[i]));
		}
	}
	timingDeschedule(machine, &sliceEvents[i]);
	roundDone[i] += (int32_t) ((uint32_t) timingCurrentTime(machine) - start);
}

// Where the next round ends: at the frame's end, at the next transfer's end, and no later than the
// shortest transfer any console could start from now.
static int32_t nextRoundEnd(void) {
	int32_t now = ROUND_LONGEST;
	for (int i = 0; i < playersHere; ++i) {
		now = roundDone[i] < now ? roundDone[i] : now;
	}
	int64_t end = ROUND_LONGEST;
	for (int i = 0; i < playersHere; ++i) {
		int64_t const shortest = (int64_t) now + sioShortestTransfer(gbas[i]);
		int32_t const left = sioTransferLeft(gbas[i]);
		end = shortest < end ? shortest : end;
		if (left > 0 && roundDone[i] + left < end) {
			end = roundDone[i] + left;
		}
	}
	return (int32_t) end;
}

static void runAdvances(void) {
	for (int i = 0; i < playersHere; ++i) {
		roundDone[i] = gbaAhead[i];
		sliceEvents[i] = (struct Event) { .callback = sliceEnd, .context = (void*) (intptr_t) i, .priority = 0x7F, .endsRun = true };
		dataWrites[i].count = 0;
		dataWrites[i].before = dataOf(i);
	}
	for (;;) {
		bool unfinished = false;
		for (int i = 0; i < playersHere; ++i) {
			unfinished |= roundDone[i] < GBA_FRAME_CYCLES;
		}
		if (!unfinished) {
			break;
		}
		roundEnd = nextRoundEnd();
		workersRun(runRound, playersHere);
		takeData();
		carryCable();
		carryBusy();
		carryAir();
		carryReady();
	}
	for (int i = 0; i < playersHere; ++i) {
		gbaAhead[i] = roundDone[i] - GBA_FRAME_CYCLES;
		for (int k = 0; k < playersHere; ++k) {
			transfers[i].takeAt[k] -= GBA_FRAME_CYCLES;
		}
	}
}

static void runAdvancesFrame(void) {
	uint16_t keys[MAX_PLAYERS];
	readControllerSwitch();
	for (int i = 0; i < playersHere; ++i) {
		int const port = portFor(i);
		keys[i] = readKeys(port) | (port ? 0 : readTurboShoulders());
		gbaSetKeys(gbas[i], keys[i]);
	}
	readScreensKeys();
	readRestarts(keys);
	if (copySaves) {
		copyFirstSave();
	}
	readSensors(keys[0]);
	runAdvances();
	updateRumble(gba->gpio.rumble);
	if (cheats.count || cheats.patchCount) {
		cheatsApply(&cheats, gba);
	}
	const void* pictures[MAX_PLAYERS];
	const int16_t* samples[MAX_PLAYERS];
	size_t frames[MAX_PLAYERS];
	for (int i = 0; i < playersHere; ++i) {
		pictures[i] = gbaShown[i];
		if (colorCorrection) {
			for (int k = 0; k < SCREEN_WIDTH * SCREEN_HEIGHT; ++k) {
				gbaCorrected[i][k] = corrected(gbaShown[i][k]);
			}
			pictures[i] = gbaCorrected[i];
		}
		samples[i] = (const int16_t*) gbas[i]->audio.out;
		frames[i] = gbas[i]->audio.outCount;
	}
	showConsoles(pictures, sizeof(uint16_t), SCREEN_WIDTH, SCREEN_HEIGHT, gbaTogetherFrame, samples, frames);
	for (int i = 0; i < playersHere; ++i) {
		gbas[i]->audio.outCount = 0;
	}
}

static void runOneGameBoy(void) {
	const int16_t* samples;
	size_t frames;
	uint16_t keys = sharedControls != SHARED_OFF ? readSharedKeys(false) : readKeys(0);
	hintSharedControls();
	readTilt(keys);
	readContrast();
	readRestarts(&keys);
	if (actionReplayLoaded) {
		libretroGbActionReplayControls(replayButtonHeld(), replaySwitch);
		keepReplayFlash(false);
	}
	const uint32_t* picture = libretroGbRunFrame(keys, &samples, &frames);
	// A restart or a loaded state can change the model.
	if (gbOnAdvance != libretroGbOnAdvance()) {
		gbOnAdvance = !gbOnAdvance;
		announceGeometry();
	}
	if (gbOnAdvance) {
		readStretch(keys);
	}
	if (gbOnAdvance && !gbCropped) {
		showPicture(advanceFrame(picture), SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint32_t));
	} else if (gbOnAdvance && gbStretched) {
		// The stretched picture, without the black above and below it.
		showPicture(advanceFrame(picture) + (SCREEN_HEIGHT - LIBRETRO_GB_HEIGHT) / 2 * SCREEN_WIDTH, SCREEN_WIDTH, LIBRETRO_GB_HEIGHT,
		             SCREEN_WIDTH * sizeof(uint32_t));
	} else {
		showPicture(picture, LIBRETRO_GB_WIDTH, LIBRETRO_GB_HEIGHT, LIBRETRO_GB_WIDTH * sizeof(uint32_t));
	}
	sendSound(samples, frames);
}

static void sendOneAdvanceSound(void) {
	audioThreadTake(gba);
	sendSound((const int16_t*) gba->audio.out, gba->audio.outCount);
	gba->audio.outCount = 0;
}

static void runOneAdvance(void) {
	uint16_t keys = sharedControls != SHARED_OFF ? readSharedKeys(true) : readKeys(0) | readTurboShoulders();
	hintSharedControls();
	readRestarts(&keys);
	gbaSetKeys(gba, keys);
	if (actionReplayLoaded) {
		actionReplayButton(replayButtonHeld());
		keepReplayFlash(false);
	}
	readSensors(keys);
	linkGbaFrame(gba);
	dolphinGbaFrame(gba);
	if (dolphinConnected() != gamecubeShown) {
		gamecubeShown = dolphinConnected();
		char message[64] = "GameCube: Dolphin has gone";
		if (gamecubeShown) {
			snprintf(message, sizeof(message), "GameCube: Dolphin at %s", dolphinAddress());
		}
		notify(message, 180);
	}
	uint32_t const frame = gba->video.frameCounter;
	do {
		gbaRunPart(gba);
	} while (gba->video.frameCounter == frame && !linkGbaHeld());
	// A sound thread mixes the last of the frame's sound while the picture is seen to.
	audioThreadKick(gba);
	if (gba->video.frameCounter == frame) {
		// Held for the link: the picture is half drawn, so the last one shows again.
		sendPicture(NULL, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
		sendOneAdvanceSound();
		return;
	}
	updateRumble(gba->gpio.rumble);
	if (cheats.count || cheats.patchCount) {
		cheatsApply(&cheats, gba);
	}
	if (colorCorrection && !frameSkipped) {
		const uint16_t* picture = (const uint16_t*) gba->renderer.frame;
		for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
			correctedFrame[i] = corrected(picture[i]);
		}
		showPicture(correctedFrame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	} else {
		showPicture(gba->renderer.frame, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
	}
	sendOneAdvanceSound();
}

// Load Save waits for netplay to end, and Back Up Save for a session this device joined, their
// options hidden meanwhile.
static void showOptionsForNetplay(void) {
	static int was;
	int const now = netplaySeen | netplayJoined << 1 | linkSession() << 2;
	if (now != was) {
		was = now;
		showOptions();
	}
}

// Which netplay client this device is, as RetroArch says; asked only once netplay has been seen, and
// not again in this game once a RetroArch that doesn't know the call has failed it in a session.
static bool askClientIndex(unsigned* client) {
	if (!netplaySeen || clientIndexUnknown) {
		return false;
	}
	bool const told = environment(RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX, client);
	clientIndexUnknown = !told && !clientTold;
	return told;
}

// A session this device joined in RetroArch ends as RetroArch stops saying which client it is. Its game,
// the host's, plays on here.
static void watchSessionEnd(void) {
	unsigned client = 0;
	// A session joined again, playing on after the last one, is watched in turn.
	if (askClientIndex(&client)) {
		clientTold = true;
		sessionOver = playingOn = false;
		return;
	}
	if (!netplayJoined || !clientTold || sessionOver) {
		return;
	}
	sessionOver = true;
	libretroSessionEnded();
	libretroPlayOn();
	notify("Netplay: the session ended; saves now go to armGBA/Remote", 360);
}

// How to use the Action Replay just loaded, once, as it starts: where its game comes from, or how its
// codes and button are reached from the core options.
static void showReplayHint(void) {
	if (replayHint == HINT_NONE || rerunning()) {
		return;
	}
	const char* const stick = replayButton == RETRO_DEVICE_ID_JOYPAD_R3   ? " (or R3)"
	                          : replayButton == RETRO_DEVICE_ID_JOYPAD_L3 ? " (or L3)"
	                                                                     : "";
	char message[300];
	switch (replayHint) {
	case HINT_NO_GAME:
		snprintf(message, sizeof(message), "%s with no game on top: pick one in Core Options > System > Game on Top, then Restart", cartridgeName());
		break;
	case HINT_GAME_BOY:
		snprintf(message, sizeof(message), "%s: switch codes on in its menu, then start the game. In the game, Core Options > System > Open Cheat Cartridge Menu%s opens its menu", cartridgeName(), stick);
		break;
	case HINT_GAME_GENIE:
		snprintf(message, sizeof(message), "Game Genie: enter up to three codes, then start the game");
		break;
	case HINT_XPLODER:
		snprintf(message, sizeof(message), "Xploder: add codes under Select Cheats, then Start Game. In the game, Core Options > System > Open Cheat Cartridge Menu%s opens its menu", stick);
		break;
	case HINT_FCD:
		snprintf(message, sizeof(message), "%s: open a game's codes with Right, switch them on with A, then start the game with Start", fcdLoaded);
		break;
	case HINT_GAME_BOY_FIRST:
		snprintf(message, sizeof(message), "Pro Action Replay: enter codes in its menu, then start the game. Core Options > System > Action Replay Switch turns them off");
		break;
	default:
		snprintf(message, sizeof(message), "%s: switch codes on in its menu, then start the game. Button codes: Core Options > System > Hold Action Replay Button%s", cartridgeName(), stick);
		break;
	}
	replayHint = HINT_NONE;
	notify(message, 480);
	logMessage(RETRO_LOG_INFO, message);
}

void retro_run(void) {
	showOptionsForNetplay();
	watchSessionEnd();
	if (sessionSaveDue) {
		sessionSaveDue = false;
		checkSessionSave();
	}
	readNewerPlayerOne();
	bool updated = false;
	if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readOptions();
		readClockOptions(true);
		saveOptionsChanged();
		readReplayPress(true);
		readCheatImport(true);
		readCheatRestore(true);
		applyCpuClock(false);
	}
	// An import waiting for the app's download of RetroArch's cheat database
	static unsigned importPoll;
	if (importWaiting && ++importPoll % 15 == 0) {
		if (cheatImportOn() && !netplaySeen) {
			importCheats();
		} else {
			importWaiting = false;
		}
	}
	if (cheatRestartDue) {
		// The cartridge reads its new list as it starts again.
		cheatRestartDue = false;
		retro_reset();
	}
	checkBooted();
	showReplayHint();
	frameSkipped = skipThisFrame();
	// One Frame Late shows the frame before, so its lines are still drawn.
	videoDrawSkipped = frameSkipped && threadedVideo != THREADED_LATE;
	// A new LCD Grid size changes the picture's largest size too, which only a new AV info can say.
	if (gridAnnounce) {
		struct retro_system_av_info av;
		retro_get_system_av_info(&av);
		environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	inputPoll();
	readL3Button();
	readFastForward();
	writeKeptSaves();
	clockFrame();
	ereaderFrame();
	if (gbTogether) {
		runGameBoys();
	} else if (gameBoy) {
		runOneGameBoy();
	} else if (gbaTogether) {
		runAdvancesFrame();
	} else {
		runOneAdvance();
	}
}

// States.

// Save states: the size, then saving and loading. A Game Boy Advance state is the machine's whole
// state; a Game Boy state is Gambatte's.
static size_t consolesStateSize(void) {
	if (gameBoy) {
		return libretroGbStateSize();
	}
	if (gbaTogether) {
		return playersHere * (sizeof(gbaAhead[0]) + gbaStateSize() + sizeof(transfers[0]) + sizeof(cables[0].ready) + sizeof(gbaShown[0]));
	}
	// An Action Replay's registers follow the console's state.
	return gbaStateSize() + (actionReplayLoaded ? actionReplayStateSize() : 0);
}

// Also tells the Game Boy core whether this state is for rollback or run-ahead, which want it exact.
// Returns whether netplay asks for this state.
static bool checkNetplay(void) {
	int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	bool const told = environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context);
	bool const forNetplay = told && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
	netplaySeen |= forNetplay;
	libretroGbExactStates(told && context != RETRO_SAVESTATE_CONTEXT_NORMAL);
	// A tag of this device's own: the time, the processor time used and where this core sits in
	// memory, which differ from one device to the next.
	while (netplaySeen && !sessionTag) {
		sessionTag = (uint32_t) time(NULL) * 2654435761u ^ (uint32_t) clock() ^ (uint32_t) (uintptr_t) &sessionTag;
	}
	return forNetplay;
}

// A state's session tag: one from another device's session, loaded by netplay, means this device
// joined it. A state loaded by hand in netplay, from a file, keeps this device's own.
static void loadedTag(uint32_t tag, bool byNetplay) {
	if (netplaySeen && !byNetplay) {
		return;
	}
	netplayJoined |= netplaySeen && tag != sessionTag;
	sessionSaveDue |= netplayJoined && byNetplay;
	sessionTag = tag;
}

int joinedPlayer(void) {
	unsigned client = 0;
	if (netplayPlayer >= 0) {
		return netplayPlayer;
	}
	return askClientIndex(&client) && client > 0 && client < MAX_PLAYERS ? (int) client : 1;
}

static bool saveConsoles(void* data, size_t size) {
	if (gameBoy) {
		return libretroGbSaveState(data, size);
	}
	if (!gba || size < consolesStateSize()) {
		return false;
	}
	if (!gbaTogether) {
		gbaSaveState(gba, data);
		if (actionReplayLoaded) {
			actionReplaySaveState(gba, (uint8_t*) data + gbaStateSize());
		}
		return true;
	}
	// The consoles here: how far each ran past the last frame, their states, the cable between them,
	// then their last pictures.
	uint8_t* at = data;
	memcpy(at, gbaAhead, playersHere * sizeof(gbaAhead[0]));
	at += playersHere * sizeof(gbaAhead[0]);
	for (int i = 0; i < playersHere; ++i) {
		gbaSaveState(gbas[i], at);
		at += gbaStateSize();
	}
	memcpy(at, transfers, playersHere * sizeof(transfers[0]));
	at += playersHere * sizeof(transfers[0]);
	for (int i = 0; i < playersHere; ++i) {
		*at++ = cables[i].ready;
	}
	memcpy(at, gbaShown, playersHere * sizeof(gbaShown[0]));
	return true;
}

static bool loadConsoles(const void* data, size_t size) {
	if (gameBoy) {
		bool loaded = libretroGbLoadState(data, size);
		checkGameBoyMemory();
		return loaded;
	}
	if (!gbaTogether && actionReplayLoaded) {
		// The Action Replay shows what the state ran with before the console's state loads; if that
		// fails, it goes back as it was.
		const uint8_t* const replay = (const uint8_t*) data + gbaStateSize();
		uint8_t* const was = gba ? malloc(actionReplayStateSize()) : NULL;
		if (!was || size < consolesStateSize()) {
			free(was);
			return false;
		}
		actionReplaySaveState(gba, was);
		bool const loaded = actionReplayPrepareState(gba, replay) && gbaLoadState(gba, data, gbaStateSize());
		const uint8_t* const now = loaded ? replay : was;
		actionReplayPrepareState(gba, now);
		actionReplayLoadState(gba, now);
		free(was);
		return loaded;
	}
	if (!gbaTogether) {
		return gba && gbaLoadState(gba, data, size);
	}
	if (size < consolesStateSize()) {
		return false;
	}
	const uint8_t* at = data;
	int32_t ahead[MAX_PLAYERS];
	memcpy(ahead, at, playersHere * sizeof(ahead[0]));
	at += playersHere * sizeof(ahead[0]);
	for (int i = 0; i < playersHere; ++i) {
		if (!gbaLoadState(gbas[i], at, gbaStateSize())) {
			return false;
		}
		at += gbaStateSize();
	}
	memcpy(gbaAhead, ahead, playersHere * sizeof(gbaAhead[0]));
	memcpy(transfers, at, playersHere * sizeof(transfers[0]));
	at += playersHere * sizeof(transfers[0]);
	for (int i = 0; i < playersHere; ++i) {
		cables[i].ready = *at++;
	}
	memcpy(gbaShown, at, playersHere * sizeof(gbaShown[0]));
	return true;
}

// A state holds, in order:
// - the consoles' states;
// - with the consoles of a link here, how long each player has held the restart buttons, so a
//   rollback restarts a console on the same frame;
// - what the buttons have done (pressed) and the sunlight level;
// - the session's tag, which tells a device in netplay that it joined another's session;
// - what Pass Around and the controls had done;
// - with one console (or each its own), how long each player has held the restart buttons, after
//   the rest so a state from before still loads: its padding, if it has any, reads as none held.
// A state from before the buttons were kept still loads, and a single-console one from before the tag.
static size_t extraStateSize(void) {
	return togetherRunning() ? sizeof(restartHeld) : 0;
}

// The state as far as the controls, and the whole of it: rounded up to a multiple of 8 bytes, as
// RetroArch's netplay goes wrong now and then with some lengths (3 past a multiple of 4: a client's
// picture jumps until its next check against the host's).
static size_t stateBeforeRestarts(void) {
	return consolesStateSize() + extraStateSize() + sizeof(pressed) + 1 + sizeof(sessionTag) + sizeof(passed) + sizeof(controls);
}

// What the stick clicks and combos do, after the restart buttons: Cheat Cartridge Button (its stick,
// or 0xFF for none), Cheat Cartridge Button Press (whether it toggles), L3 Button and Restart Combo;
// the CPU Clock; and the turbo buttons, L2 and R2 as Turbo L and R and Turbo Button Period.
// Each device's own options set them, but in netplay they shape what the state holds, so a state that
// netplay loads brings the host's.
struct SharedControls {
	uint8_t replayButton, replayToggles, l3Button, restartButtons;
	// The CPU Clock (cpuOverclock) and CPU Clock During Boot, which each device's emulation must share
	int8_t cpuOverclock;
	uint8_t stockDuringBoot;
	uint8_t turboTriggers, turboPeriod;
};

// Last, the CRC-32 of the cheat cartridge's file (replayCartCrc). A state holds the cartridge's chip as
// it differs from that image, which every player has, so a state only loads with the same one.
static size_t stateUsed(void) {
	return stateBeforeRestarts() + sizeof(restartHeld) + sizeof(struct SharedControls) + sizeof(replayCartCrc);
}

size_t retro_serialize_size(void) {
	return (stateUsed() + 7) & ~(size_t) 7;
}

bool retro_serialize(void* data, size_t size) {
	checkNetplay();
	if (size < retro_serialize_size() || !saveConsoles(data, consolesStateSize())) {
		return false;
	}
	uint8_t* at = (uint8_t*) data + consolesStateSize();
	memcpy(at, restartHeld, extraStateSize());
	at += extraStateSize();
	memcpy(at, &pressed, sizeof(pressed));
	at += sizeof(pressed);
	*at++ = (uint8_t) solarLevel;
	memcpy(at, &sessionTag, sizeof(sessionTag));
	memcpy(at + sizeof(sessionTag), &passed, sizeof(passed));
	memcpy(at + sizeof(sessionTag) + sizeof(passed), &controls, sizeof(controls));
	// Linked consoles keep theirs before the buttons; written here too, and read only when not linked
	memcpy((uint8_t*) data + stateBeforeRestarts(), restartHeld, sizeof(restartHeld));
	struct SharedControls const shared = {
		replayButton == REPLAY_BUTTON_OFF ? 0xFF : (uint8_t) replayButton, replayToggles, (uint8_t) l3Button, (uint8_t) restartButtons,
		cpuOverclock, stockDuringBoot, turboOnTriggers, (uint8_t) turboPeriod,
	};
	memcpy((uint8_t*) data + stateBeforeRestarts() + sizeof(restartHeld), &shared, sizeof(shared));
	memcpy((uint8_t*) data + stateBeforeRestarts() + sizeof(restartHeld) + sizeof(shared), &replayCartCrc, sizeof(replayCartCrc));
	memset((uint8_t*) data + stateUsed(), 0, retro_serialize_size() - stateUsed());
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	bool const byNetplay = checkNetplay();
	// A state from before Pass Around was kept is as long as this one without it, and one from before
	// the Action Replay's button and L3 were, without them.
	size_t const controlled = stateBeforeRestarts(), buttons = sizeof(pressed) + 1;
	size_t const unpassed = controlled - sizeof(controls) - sizeof(passed);
	bool const full = size >= unpassed, older = size == unpassed - buttons, holding = size >= unpassed + sizeof(passed);
	bool const tagged = full || older;
	// With a cheat cartridge, only a state made with the same one: its chip is held as it differs from
	// the cartridge's image
	if (actionReplayLoaded) {
		size_t const at = controlled + sizeof(restartHeld) + sizeof(struct SharedControls);
		uint32_t crc = 0;
		if (size >= at + sizeof(crc)) {
			memcpy(&crc, (const uint8_t*) data + at, sizeof(crc));
		}
		if (crc != replayCartCrc) {
			char message[200];
			snprintf(message, sizeof(message), "This state was made with another copy of the %s: every player needs the same one", cartridgeName());
			notify(message, 360);
			logMessage(RETRO_LOG_WARN, message);
			return false;
		}
	}
	if (!(tagged || (size == consolesStateSize() && !togetherRunning())) || !loadConsoles(data, consolesStateSize())) {
		return false;
	}
	// In netplay the CPU Clock is the host's, as every device must run the same machine
	if (byNetplay && size >= controlled + sizeof(restartHeld) + sizeof(struct SharedControls)) {
		struct SharedControls shared;
		memcpy(&shared, (const uint8_t*) data + controlled + sizeof(restartHeld), sizeof(shared));
		if (shared.cpuOverclock >= -3 && shared.cpuOverclock <= 12 && shared.stockDuringBoot <= 1) {
			cpuOverclock = shared.cpuOverclock;
			stockDuringBoot = shared.stockDuringBoot;
		}
	}
	// Whether the console is still booting, for CPU Clock During Boot, is the state's, not this run's:
	// a frame run again from before the game started runs at the boot's clock, as it did.
	booting = stillBooting();
	applyCpuClock(true);
	// A loaded state has every console's save as it was; player 1's isn't copied over them after.
	copySaves = 0;
	if (tagged) {
		const uint8_t* at = (const uint8_t*) data + consolesStateSize();
		memcpy(restartHeld, at, extraStateSize());
		at += extraStateSize();
		if (full) {
			memcpy(&pressed, at, sizeof(pressed));
			solarLevel = at[sizeof(pressed)] <= 10 ? at[sizeof(pressed)] : solarLevel;
			at += buttons;
		} else {
			memset(&pressed, 0, sizeof(pressed));
		}
		uint32_t tag;
		memcpy(&tag, at, sizeof(tag));
		loadedTag(tag, byNetplay);
		if (holding) {
			memcpy(&passed, at + sizeof(tag), sizeof(passed));
		} else {
			memset(&passed, 0, sizeof(passed));
		}
		if (size >= controlled) {
			memcpy(&controls, at + sizeof(tag) + sizeof(passed), sizeof(controls));
		} else {
			memset(&controls, 0, sizeof(controls));
		}
		// Without it, a rollback in netplay would leave the hold counted on, and restart the console
		// on another frame than the other devices'.
		if (!togetherRunning()) {
			if (size >= controlled + sizeof(restartHeld)) {
				memcpy(restartHeld, (const uint8_t*) data + controlled, sizeof(restartHeld));
			} else {
				memset(restartHeld, 0, sizeof(restartHeld));
			}
		}
		struct SharedControls shared;
		if (byNetplay && size >= controlled + sizeof(restartHeld) + sizeof(shared)) {
			memcpy(&shared, (const uint8_t*) data + controlled + sizeof(restartHeld), sizeof(shared));
			bool const known = (shared.replayButton == 0xFF || shared.replayButton == RETRO_DEVICE_ID_JOYPAD_L3 ||
			                    shared.replayButton == RETRO_DEVICE_ID_JOYPAD_R3) &&
			                   shared.l3Button <= L3_OFF && shared.restartButtons <= RESTART_OFF && shared.turboTriggers <= 1 &&
			                   shared.turboPeriod >= TURBO_FEWEST && shared.turboPeriod <= TURBO_MOST;
			if (known) {
				replayButton = shared.replayButton == 0xFF ? REPLAY_BUTTON_OFF : shared.replayButton;
				replayToggles = shared.replayToggles != 0;
				l3Button = shared.l3Button;
				restartButtons = shared.restartButtons;
				turboPeriod = shared.turboPeriod;
				turboPulse = clampInt(turboPeriod / 2, 2, 15);
				if (turboOnTriggers != (shared.turboTriggers != 0)) {
					turboOnTriggers = shared.turboTriggers != 0;
					showButtons();
				}
			}
		}
	}
	return true;
}

// Cheats, and the memory the frontend sees.

static void applyCheatCodes(void) {
	if (gameBoy) {
		if (!libretroGbSetCheats((const char* const*) cheatCodes, cheatCodeCount)) {
			logMessage(RETRO_LOG_WARN, "armGBA: a cheat code is neither Game Genie nor GameShark");
		}
		return;
	}
	cheatsClear(&cheats, gba);
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		if (cheatCodes[i] && !cheatsAdd(&cheats, cheatCodes[i])) {
			char message[160];
			snprintf(message, sizeof(message), "armGBA: cheat code %.100s isn't one armGBA reads", cheatCodes[i]);
			logMessage(RETRO_LOG_WARN, message);
		}
	}
}

// Cheat codes: GameShark, Action Replay and CodeBreaker for the Game Boy Advance; Game Genie and
// GameShark for the Game Boy.
void retro_cheat_reset(void) {
	for (size_t i = 0; i < cheatCodeCount; ++i) {
		free(cheatCodes[i]);
	}
	free(cheatCodes);
	cheatCodes = NULL;
	cheatCodeCount = 0;
	applyCheatCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	if (index >= cheatCodeCount) {
		char** grown = realloc(cheatCodes, (index + 1) * sizeof(*cheatCodes));
		if (!grown) {
			return;
		}
		memset(&grown[cheatCodeCount], 0, (index + 1 - cheatCodeCount) * sizeof(*grown));
		cheatCodes = grown;
		cheatCodeCount = index + 1;
	}
	free(cheatCodes[index]);
	cheatCodes[index] = enabled && code ? strdup(code) : NULL;
	applyCheatCodes();
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

// Memory the frontend may read or keep, and its size:
// - the cartridge's save memory, loaded and saved as the save file;
// - the cartridge's clock, as the .rtc file;
// - work RAM, for achievements and cheats.
// RetroArch gets no save memory when the core keeps player 1's save itself.
static void* memory(unsigned id, size_t* size) {
	*size = 0;
	if (pairFile && id != RETRO_MEMORY_SYSTEM_RAM) {
		return NULL;
	}
	if (actionReplayLoaded && (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC)) {
		// The core keeps the Action Replay's flash, and the game on top's save under the game's
		// name.
		return NULL;
	}
	if (!retroArchSaves() && (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC)) {
		return NULL;
	}
	if (gbTogether && (id == MEMORY_SECOND_SAVE || id == MEMORY_SECOND_RTC)) {
		libretroGbSelect(1);
		void* const data = id == MEMORY_SECOND_SAVE ? libretroGbSaveRam(size) : libretroGbRtc(size);
		libretroGbSelect(0);
		return data;
	}
	if (gameBoy) {
		return id == RETRO_MEMORY_SAVE_RAM ? libretroGbSaveRam(size) : id == RETRO_MEMORY_SYSTEM_RAM ? libretroGbSystemRam(size)
		     : id == RETRO_MEMORY_RTC                                ? libretroGbRtc(size)
		                                                             : NULL;
	}
	if (!gba) {
		return NULL;
	}
	switch (id) {
	case MEMORY_SECOND_SAVE:
		if (!gbaTogether) {
			return NULL;
		}
		*size = saveSize(gbas[1]);
		return gbas[1]->savedata.storage;
	case RETRO_MEMORY_SAVE_RAM:
		*size = saveSize(gba);
		return gba->savedata.storage;
	case MEMORY_SECOND_RTC:
		return gbaTogether ? clockMemory(1, size) : NULL;
	case RETRO_MEMORY_RTC:
		return clockMemory(0, size);
	case RETRO_MEMORY_SYSTEM_RAM:
		*size = SIZE_EWRAM;
		return gba->ewram;
	default:
		return NULL;
	}
}

void* retro_get_memory_data(unsigned id) {
	size_t size;
	return memory(id, &size);
}

size_t retro_get_memory_size(unsigned id) {
	size_t size;
	memory(id, &size);
	return size;
}
