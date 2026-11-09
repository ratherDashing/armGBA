/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro side: the environment the core asks about, the game it runs, and its saves. Save
// files are kept as RetroArch keeps them (<game>.srm and .rtc in the save folder), so they move
// between this and RetroArch as they are.

#include "frontend.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"

// The screen the core's CPU LCD Effects fit when LCD Scale is Fit Screen (libretro/lcd.c): the top
// screen, as the core's own frontend tells it its window.
extern unsigned libretroLcdScreenWidth, libretroLcdScreenHeight;

uint32_t gameFrames;
static bool loaded;
static char gamePath[512];
static char gameName[256];
static void* gameData;
static struct retro_system_av_info avInfo;
static retro_audio_buffer_status_callback_t audioStatus;
static retro_core_options_update_display_callback_t updateDisplay;
static struct retro_fastforwarding_override fastForward;
static uint32_t savedHash[2];
static uint32_t saveCheckFrame;
static char loadError[160];

const char* gameLoadError(void) {
	return loadError;
}

static void coreLog(enum retro_log_level level, const char* format, ...) {
	static const char* const levels[] = { "debug", "info", "warn", "error" };
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	line[strcspn(line, "\n")] = 0;
	logLine("core %s: %s", levels[level & 3], line);
}

static bool environment(unsigned cmd, void* data) {
	switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = coreLog;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = SYSTEM_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = SAVE_DIR;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format*) data;
		if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_0RGB1555) {
			return false;
		}
		displaySetFormat(f);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		if (data) {
			optionsDefineV2(data);
		}
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		if (data) {
			optionsDefineV0(data);
		}
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		v->value = optionValue(v->key);
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = feOptionsChanged;
		feOptionsChanged = false;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLE: {
		const struct retro_variable* v = data;
		if (v && v->key && v->value) {
			optionsApplySaved(v->key, v->value);
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		if (data) {
			const struct retro_core_option_display* d = data;
			optionSetVisible(d->key, d->visible);
		}
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
		updateDisplay = data ? ((const struct retro_core_options_update_display_callback*) data)->callback : NULL;
		return true;
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
		audioStatus = data ? ((const struct retro_audio_buffer_status_callback*) data)->callback : NULL;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*(int*) data = 3;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		avInfo.geometry = *(const struct retro_game_geometry*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		avInfo = *(const struct retro_system_av_info*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message* m = data;
		if (m && m->msg) {
			logLine("message: %s", m->msg);
			menuMessage(m->msg, m->frames);
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
		if (data) {
			fastForward = *(const struct retro_fastforwarding_override*) data;
		}
		return true;
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		return true;
	default:
		// No rumble, netplay, network packets or save state contexts here.
		return false;
	}
}

static void coreVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	displayFrame(data, width, height, pitch);
}

static int16_t sampleBuffer[64];
static size_t sampleCount;

static void coreAudioSample(int16_t left, int16_t right) {
	sampleBuffer[sampleCount++] = left;
	sampleBuffer[sampleCount++] = right;
	if (sampleCount == sizeof(sampleBuffer) / sizeof(*sampleBuffer)) {
		soundBatch(sampleBuffer, sampleCount / 2);
		sampleCount = 0;
	}
}

static size_t coreAudioSampleBatch(const int16_t* data, size_t frames) {
	return soundBatch(data, frames);
}

static void inputPollCore(void) {
	// Polled once a frame by the main loop already.
}

static int16_t inputStateCore(unsigned port, unsigned device, unsigned index, unsigned id) {
	return inputState(port, device, index, id);
}

void gameRefreshOptionDisplay(void) {
	if (updateDisplay) {
		updateDisplay();
	}
}

void gameOptionsChanged(void) {
	feOptionsChanged = true;
	if (updateDisplay) {
		updateDisplay();
	}
}

void gameCoreInit(void) {
	libretroLcdScreenWidth = 400;
	libretroLcdScreenHeight = 240;
	retro_set_environment(environment);
	retro_set_video_refresh(coreVideoRefresh);
	retro_set_audio_sample(coreAudioSample);
	retro_set_audio_sample_batch(coreAudioSampleBatch);
	retro_set_input_poll(inputPollCore);
	retro_set_input_state(inputStateCore);
	retro_init();
}

void gameCoreExit(void) {
	retro_deinit();
}

static void filePath(char* out, size_t size, const char* dir, const char* extension) {
	snprintf(out, size, "%s/%s.%s", dir, gameName, extension);
}

// A cheap hash of save memory, to tell when the game has written to it.
static uint32_t hashOf(const uint8_t* data, size_t size) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < size; ++i) {
		h = (h ^ data[i]) * 16777619u;
	}
	return h;
}

static void readSave(unsigned id, const char* extension) {
	size_t const size = retro_get_memory_size(id);
	uint8_t* memory = retro_get_memory_data(id);
	if (!size || !memory) {
		return;
	}
	char path[600];
	filePath(path, sizeof(path), SAVE_DIR, extension);
	FILE* f = fopen(path, "rb");
	if (f) {
		size_t got = fread(memory, 1, size, f);
		fclose(f);
		logLine("save: read %u of %u bytes from %s", (unsigned) got, (unsigned) size, path);
	}
	savedHash[id == RETRO_MEMORY_RTC] = hashOf(memory, size);
}

static void writeSave(unsigned id, const char* extension, bool force) {
	size_t const size = retro_get_memory_size(id);
	uint8_t* memory = retro_get_memory_data(id);
	if (!size || !memory) {
		return;
	}
	uint32_t const h = hashOf(memory, size);
	if (!force && h == savedHash[id == RETRO_MEMORY_RTC]) {
		return;
	}
	char path[600];
	filePath(path, sizeof(path), SAVE_DIR, extension);
	if (writeFile(path, memory, size)) {
		savedHash[id == RETRO_MEMORY_RTC] = h;
		logLine("save: wrote %s", path);
	} else {
		logLine("save: couldn't write %s", path);
	}
}

// Writes save memory once it has changed (checked every two seconds; a game writes its save over
// several frames, which this lets finish), or now with force.
void gameSaveSram(bool force) {
	if (!loaded) {
		return;
	}
	if (!force && gameFrames - saveCheckFrame < 120) {
		return;
	}
	saveCheckFrame = gameFrames;
	writeSave(RETRO_MEMORY_SAVE_RAM, "srm", false);
	writeSave(RETRO_MEMORY_RTC, "rtc", false);
}

bool gameLoad(const char* path) {
	snprintf(gamePath, sizeof(gamePath), "%s", path);
	baseName(path, gameName, sizeof(gameName));
	char inner[256] = "";
	size_t size = 0;
	bool offered = false;
	extern u32 __ctru_heap_size;
	struct mallinfo before = mallinfo();
	logLine("load: %s, %lu of %lu KiB of heap in use", path, (unsigned long) before.uordblks >> 10, (unsigned long) __ctru_heap_size >> 10);
	gameData = romLoad(path, &size, inner, sizeof(inner), &offered);
	if (!gameData) {
		snprintf(loadError, sizeof(loadError), "Couldn't read the game, or no memory for it (heap %lu MiB). See log.txt",
		         (unsigned long) __ctru_heap_size >> 20);
		logLine("load: can't read %s", path);
		return false;
	}
	logLine("load: %s (%s, %u bytes%s)", path, inner, (unsigned) size, offered ? ", cartridge buffer" : "");
	// A game from an archive is "<archive>#<game>", as RetroArch names it: the core tells a Game Boy
	// game by the extension after the '#', and names its files after the archive.
	char corePath[800];
	const char* slash = strrchr(path, '/');
	bool const archived = strcmp(slash ? slash + 1 : path, inner) != 0;
	snprintf(corePath, sizeof(corePath), archived ? "%s#%s" : "%s", path, inner);
	struct retro_game_info info = { .path = corePath, .data = gameData, .size = size, .meta = NULL };
	if (offered) {
		gbaOfferRom(gameData);
	}
	bool ok = retro_load_game(&info);
	if (offered) {
		if (gbaOfferedRom() == NULL) {
			// Taken over by the core, which frees it.
			gameData = NULL;
		}
		gbaOfferRom(NULL);
	}
	free(gameData);
	gameData = NULL;
	if (!ok) {
		snprintf(loadError, sizeof(loadError), "The core couldn't start it (heap %lu MiB; recompiler memory?). See log.txt",
		         (unsigned long) __ctru_heap_size >> 20);
		logLine("load: the core refused %s", path);
		return false;
	}
	retro_get_system_av_info(&avInfo);
	logLine("load: %ux%u, %.4f fps, %.0f Hz", avInfo.geometry.base_width, avInfo.geometry.base_height, avInfo.timing.fps,
	     avInfo.timing.sample_rate);
	// The heap is all the app's memory that isn't linear: on an Old 3DS, about 30 MiB of it is the
	// cartridge buffer and 6 MiB the recompiler's code.
	extern u32 __ctru_heap_size;
	struct mallinfo heap = mallinfo();
	logLine("memory: %lu of %lu KiB of heap in use, %lu KiB linear free", (unsigned long) heap.uordblks >> 10,
	        (unsigned long) __ctru_heap_size >> 10, (unsigned long) (linearSpaceFree() >> 10));
	loaded = true;
	gameFrames = 0;
	saveCheckFrame = 0;
	readSave(RETRO_MEMORY_SAVE_RAM, "srm");
	readSave(RETRO_MEMORY_RTC, "rtc");
	// The game loop runs at the GBA's own frame rate (main.c), so the sound plays at its own rate.
	soundInit(avInfo.timing.sample_rate);
	return true;
}

void gameUnload(void) {
	if (!loaded) {
		return;
	}
	gameSaveSram(true);
	retro_unload_game();
	soundExit();
	loaded = false;
	displayClearTop();
}

void gameRunFrame(void) {
	if (audioStatus) {
		unsigned const occupancy = soundOccupancy();
		audioStatus(true, occupancy, occupancy < 25);
	}
	retro_run();
	++gameFrames;
	if (sampleCount) {
		soundBatch(sampleBuffer, sampleCount / 2);
		sampleCount = 0;
	}
}

double gameFps(void) {
	return avInfo.timing.fps > 1 ? avInfo.timing.fps : 59.7275;
}

bool gameFastForward(void) {
	return fastForward.fastforward;
}

void gameReset(void) {
	retro_reset();
}

const char* gameTitle(void) {
	return gameName;
}

static void statePath(char* out, size_t size, int slot) {
	char extension[16];
	snprintf(extension, sizeof(extension), slot ? "state%d" : "state", slot);
	filePath(out, size, STATE_DIR, extension);
}

bool gameStateExists(int slot) {
	char path[600];
	statePath(path, sizeof(path), slot);
	return fileExists(path);
}

bool gameSaveState(int slot) {
	size_t const size = retro_serialize_size();
	void* data = size ? malloc(size) : NULL;
	bool ok = data && retro_serialize(data, size);
	char path[600];
	statePath(path, sizeof(path), slot);
	ok = ok && writeFile(path, data, size);
	free(data);
	logLine("state: save to %s %s", path, ok ? "done" : "FAILED");
	return ok;
}

bool gameLoadState(int slot) {
	char path[600];
	statePath(path, sizeof(path), slot);
	size_t size = 0;
	void* data = readFile(path, &size);
	bool ok = data && retro_unserialize(data, size);
	free(data);
	logLine("state: load from %s %s", path, ok ? "done" : "FAILED");
	return ok;
}
