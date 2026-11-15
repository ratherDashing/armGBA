/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <time.h>
#include "frontend.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE* logFile;
static u64 logStart;

void logInit(void) {
	makeDirs();
	// The last run's log is kept as log-prev.txt: when the app hung or crashed, that's the one wanted.
	remove(APP_DIR "/log-prev2.txt");
	rename(APP_DIR "/log-prev.txt", APP_DIR "/log-prev2.txt");
	rename(LOG_FILE, APP_DIR "/log-prev.txt");
	logFile = fopen(LOG_FILE, "w");
	logStart = osGetTime();
}

// --- SD card work off the game's thread ---------------------------------------------------------
//
// Every call that reaches the SD card takes the console tens of milliseconds, and a file written
// whole (made, written, closed, renamed) several of them: on the game's thread that is frames lost.
// While a game runs, log lines and the game's save files go to a thread of their own instead (New
// 3DS: on core 2, where it mostly waits for the card). Without the thread, and outside a game, they
// are written at once as before.
static LightLock ioLock;
static LightEvent ioWake;
static Thread ioThread;
static bool ioUp, ioQuit, ioDeferring;
static volatile bool ioBusy;
static char ioText[8192];
static size_t ioTextSize;
static struct {
	char path[600];
	void* data;
	size_t size;
} ioJobs[2];

static void ioMain(void* unused) {
	(void) unused;
	static char text[sizeof(ioText)];
	static char path[600];
	for (;;) {
		LightEvent_Wait(&ioWake);
		for (;;) {
			void* data = NULL;
			size_t size = 0;
			LightLock_Lock(&ioLock);
			size_t const n = ioTextSize;
			memcpy(text, ioText, n);
			ioTextSize = 0;
			for (int i = 0; i < 2 && !data; ++i) {
				if (ioJobs[i].data) {
					data = ioJobs[i].data;
					size = ioJobs[i].size;
					memcpy(path, ioJobs[i].path, sizeof(path));
					ioJobs[i].data = NULL;
				}
			}
			if (!n && !data) {
				ioBusy = false;
				LightLock_Unlock(&ioLock);
				break;
			}
			LightLock_Unlock(&ioLock);
			if (n && logFile) {
				fwrite(text, 1, n, logFile);
				fflush(logFile);
			}
			if (data) {
				bool const ok = writeFile(path, data, size);
				free(data);
				logLine(ok ? "save: wrote %s" : "save: couldn't write %s", path);
			}
		}
		if (ioQuit) {
			return;
		}
	}
}

void ioStart(void) {
	LightLock_Init(&ioLock);
	LightEvent_Init(&ioWake, RESET_ONESHOT);
	if (!isNew3ds()) {
		return;
	}
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	ioThread = threadCreate(ioMain, NULL, 16 * 1024, priority - 1, 2, false);
	ioUp = ioThread != NULL;
}

// Waits until everything handed to the thread is on the card.
void ioFlush(void) {
	while (ioUp && ioBusy) {
		LightEvent_Signal(&ioWake);
		svcSleepThread(1000000);
	}
}

void ioStop(void) {
	if (!ioUp) {
		return;
	}
	ioFlush();
	ioQuit = true;
	LightEvent_Signal(&ioWake);
	threadJoin(ioThread, U64_MAX);
	threadFree(ioThread);
	ioUp = false;
}

// While a game runs (on): log lines are written by the thread. Off: at once, as a crash needs.
void logDefer(bool on) {
	if (!on) {
		ioFlush();
	}
	ioDeferring = on && ioUp;
}

// A file written whole by the thread, from a copy of data; slot 0 or 1, each holding the latest
// asked for. Without the thread, written now. False when it couldn't be (written or copied).
bool writeFileLater(int slot, const char* path, const void* data, size_t size) {
	void* copy = ioUp && ioDeferring ? malloc(size ? size : 1) : NULL;
	if (!copy) {
		ioFlush();
		bool const ok = writeFile(path, data, size);
		logLine(ok ? "save: wrote %s" : "save: couldn't write %s", path);
		return ok;
	}
	memcpy(copy, data, size);
	LightLock_Lock(&ioLock);
	free(ioJobs[slot].data);
	ioJobs[slot].data = copy;
	ioJobs[slot].size = size;
	snprintf(ioJobs[slot].path, sizeof(ioJobs[slot].path), "%s", path);
	ioBusy = true;
	LightLock_Unlock(&ioLock);
	LightEvent_Signal(&ioWake);
	return true;
}

// One line to log.txt, flushed so that a crash leaves it behind; while a game runs, by the thread
// above.
void logLine(const char* format, ...) {
	if (!logFile) {
		return;
	}
	va_list args;
	va_start(args, format);
	if (ioUp && (ioDeferring || threadGetCurrent() == ioThread)) {
		char line[1200];
		int n = snprintf(line, sizeof(line), "[%7.2f] ", (osGetTime() - logStart) / 1000.0);
		n += vsnprintf(line + n, sizeof(line) - n - 1, format, args);
		va_end(args);
		n = n > (int) sizeof(line) - 2 ? (int) sizeof(line) - 2 : n;
		line[n++] = '\n';
		LightLock_Lock(&ioLock);
		if (ioTextSize + n <= sizeof(ioText)) {
			memcpy(ioText + ioTextSize, line, n);
			ioTextSize += n;
		}
		ioBusy = true;
		LightLock_Unlock(&ioLock);
		LightEvent_Signal(&ioWake);
		return;
	}
	ioFlush();
	fprintf(logFile, "[%7.2f] ", (osGetTime() - logStart) / 1000.0);
	vfprintf(logFile, format, args);
	va_end(args);
	fputc('\n', logFile);
	fflush(logFile);
}

void logClose(void) {
	ioStop();
	if (logFile) {
		fclose(logFile);
		logFile = NULL;
	}
}

// Azahar and Citra answer a system info query of their own that a real 3DS refuses (Citra with 1,
// Azahar with its own number).
bool isEmulator(void) {
	static int known = -1;
	if (known < 0) {
		s64 value = 0;
		known = R_SUCCEEDED(svcGetSystemInfo(&value, 0x20000, 0)) && value != 0;
	}
	return known;
}

bool isNew3ds(void) {
	bool isNew = false;
	APT_CheckNew3DS(&isNew);
	return isNew;
}

void makeDirs(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	mkdir(SYSTEM_DIR, 0777);
	mkdir(SAVE_DIR, 0777);
	mkdir(STATE_DIR, 0777);
	mkdir(WARM_DIR, 0777);
}

// Writes to a new file then renames it over the old one, so a power cut can't leave half a save.
bool writeFile(const char* path, const void* data, size_t size) {
	char temp[600];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	FILE* f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(temp);
		return false;
	}
	remove(path);
	return rename(temp, path) == 0;
}

void* readFile(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = length >= 0 ? malloc(length ? (size_t) length : 1) : NULL;
	if (data && fread(data, 1, (size_t) length, f) != (size_t) length) {
		free(data);
		data = NULL;
	}
	fclose(f);
	if (data) {
		*size = (size_t) length;
	}
	return data;
}

bool fileExists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

void baseName(const char* path, char* out, size_t size) {
	const char* slash = strrchr(path, '/');
	snprintf(out, size, "%s", slash ? slash + 1 : path);
	char* dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = 0;
	}
}

static void put16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void put32(uint8_t* p, uint32_t v) {
	put16(p, v);
	put16(p + 2, v >> 16);
}

// A 24-bit BMP, rows bottom up; pixel(x, y) gives RGB.
static bool writeBmp(const char* path, int width, int height, void (*pixel)(void*, int, int, uint8_t*), void* context) {
	int const rowBytes = (width * 3 + 3) & ~3;
	uint32_t const imageBytes = rowBytes * height;
	uint8_t header[54] = { 'B', 'M' };
	put32(header + 2, sizeof(header) + imageBytes);
	put32(header + 10, sizeof(header));
	put32(header + 14, 40);
	put32(header + 18, width);
	put32(header + 22, height);
	put16(header + 26, 1);
	put16(header + 28, 24);
	put32(header + 34, imageBytes);
	uint8_t* image = calloc(1, imageBytes);
	if (!image) {
		return false;
	}
	for (int y = 0; y < height; ++y) {
		uint8_t* row = image + (height - 1 - y) * rowBytes;
		for (int x = 0; x < width; ++x) {
			uint8_t rgb[3];
			pixel(context, x, y, rgb);
			row[x * 3] = rgb[2];
			row[x * 3 + 1] = rgb[1];
			row[x * 3 + 2] = rgb[0];
		}
	}
	FILE* f = fopen(path, "wb");
	bool ok = f && fwrite(header, sizeof(header), 1, f) == 1 && fwrite(image, imageBytes, 1, f) == 1;
	if (f) {
		ok = fclose(f) == 0 && ok;
	}
	free(image);
	return ok;
}

struct Rgb565 {
	const uint16_t* pixels;
	int pitch;
};

static void rgb565Pixel(void* context, int x, int y, uint8_t* rgb) {
	struct Rgb565* c = context;
	uint16_t p = c->pixels[y * c->pitch + x];
	rgb[0] = (p >> 11) << 3 | (p >> 13);
	rgb[1] = ((p >> 5) & 0x3F) << 2 | ((p >> 9) & 3);
	rgb[2] = (p & 0x1F) << 3 | ((p >> 2) & 7);
}

bool writeBmp565(const char* path, const uint16_t* pixels, int width, int height, int pitch) {
	struct Rgb565 c = { pixels, pitch };
	return writeBmp(path, width, height, rgb565Pixel, &c);
}

// The framebuffers are stored rotated: column-major from the bottom left, 240 pixels to a column.
// The top screen is 800 columns in wide mode (LCD3x), else 400; the bottom one is centered under it.
static void screenPixel(void* context, int x, int y, uint8_t* rgb) {
	int const topWidth = *(const int*) context;
	gfxScreen_t screen = y < 240 ? GFX_TOP : GFX_BOTTOM;
	int width = y < 240 ? topWidth : 320;
	int sx = y < 240 ? x : x - (topWidth - 320) / 2;
	int sy = y < 240 ? y : y - 240;
	rgb[0] = rgb[1] = rgb[2] = 0;
	if (sx < 0 || sx >= width) {
		return;
	}
	const uint8_t* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	int const index = sx * 240 + (239 - sy);
	switch (gfxGetScreenFormat(screen)) {
	case GSP_RGBA8_OES:
		rgb[0] = fb[index * 4 + 3];
		rgb[1] = fb[index * 4 + 2];
		rgb[2] = fb[index * 4 + 1];
		break;
	case GSP_BGR8_OES:
		rgb[0] = fb[index * 3 + 2];
		rgb[1] = fb[index * 3 + 1];
		rgb[2] = fb[index * 3];
		break;
	case GSP_RGB565_OES: {
		uint16_t p = ((const uint16_t*) fb)[index];
		rgb[0] = (p >> 11) << 3;
		rgb[1] = ((p >> 5) & 0x3F) << 2;
		rgb[2] = (p & 0x1F) << 3;
		break;
	}
	default:
		break;
	}
}

bool screenshotScreens(const char* path) {
	int topWidth = gfxIsWide() ? 800 : 400;
	return writeBmp(path, topWidth, 480, screenPixel, &topWidth);
}

// newlib has no timegm: days from the civil date (Howard Hinnant's algorithm), as UTC.
time_t timegm(struct tm* tm) {
	long y = tm->tm_year + 1900L + tm->tm_mon / 12;
	int m = tm->tm_mon % 12;
	if (m < 0) {
		m += 12;
		--y;
	}
	y -= m < 2;
	long const era = (y >= 0 ? y : y - 399) / 400;
	long const yoe = y - era * 400;
	long const doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + tm->tm_mday - 1;
	long const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long const days = era * 146097LL + doe - 719468;
	return (time_t) (days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}
