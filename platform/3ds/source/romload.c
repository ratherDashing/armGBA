/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Reads a game: a plain file, the first game in a .zip (inflated here, straight into its buffer), or
// in a .7z (through libretro-common's archive code). A GBA game goes into a zeroed buffer the size of
// the whole cartridge space, which the core takes over instead of copying: a 32 MiB game would
// otherwise need 64 MiB, all an Old 3DS has.

#include "frontend.h"

#include <file/archive_file.h>
#include <lists/string_list.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "gba.h"

static const char* extensionOf(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot ? dot + 1 : "";
}

static bool isGbaName(const char* name) {
	const char* ext = extensionOf(name);
	return !strcasecmp(ext, "gba") || !strcasecmp(ext, "agb") || !strcasecmp(ext, "bin") || !strcasecmp(ext, "mb");
}

static bool isGameName(const char* name) {
	const char* ext = extensionOf(name);
	return isGbaName(name) || !strcasecmp(ext, "gb") || !strcasecmp(ext, "gbc") || !strcasecmp(ext, "sgb");
}

// A buffer for a game of size bytes: the cartridge space for a GBA game.
static uint8_t* gameBuffer(const char* name, size_t size, bool* offered) {
	*offered = false;
	if (isGbaName(name) && size <= SIZE_ROM) {
		uint8_t* buffer = calloc(1, SIZE_ROM);
		*offered = buffer != NULL;
		if (!buffer) {
			logLine("load: no memory for the 32 MiB cartridge buffer");
		}
		return buffer;
	}
	return malloc(size ? size : 1);
}

static uint16_t le16(const uint8_t* p) {
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t* p) {
	return le16(p) | (uint32_t) le16(p + 2) << 16;
}

static void* zipLoad(FILE* f, size_t* size, char* innerName, size_t innerSize, bool* offered) {
	// The end of central directory record, in the last 64 KiB.
	fseek(f, 0, SEEK_END);
	long const fileSize = ftell(f);
	long const tail = fileSize < 65557 ? fileSize : 65557;
	uint8_t* end = malloc(tail);
	if (!end) {
		return NULL;
	}
	fseek(f, fileSize - tail, SEEK_SET);
	if (fread(end, 1, tail, f) != (size_t) tail) {
		free(end);
		return NULL;
	}
	long eocd = -1;
	for (long i = tail - 22; i >= 0; --i) {
		if (le32(end + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		free(end);
		return NULL;
	}
	uint32_t const entries = le16(end + eocd + 10);
	uint32_t const dirSize = le32(end + eocd + 12);
	uint32_t const dirOffset = le32(end + eocd + 16);
	free(end);
	uint8_t* dir = malloc(dirSize);
	if (!dir) {
		return NULL;
	}
	fseek(f, dirOffset, SEEK_SET);
	if (fread(dir, 1, dirSize, f) != dirSize) {
		free(dir);
		return NULL;
	}
	// The first game in it.
	uint32_t method = 0, compressed = 0, uncompressed = 0, local = 0;
	bool found = false;
	for (uint32_t i = 0, at = 0; i < entries && at + 46 <= dirSize; ++i) {
		const uint8_t* e = dir + at;
		if (le32(e) != 0x02014b50) {
			break;
		}
		uint32_t const nameLength = le16(e + 28);
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int) (nameLength < 255 ? nameLength : 255), (const char*) e + 46);
		if (isGameName(name)) {
			method = le16(e + 10);
			compressed = le32(e + 20);
			uncompressed = le32(e + 24);
			local = le32(e + 42);
			snprintf(innerName, innerSize, "%s", name);
			found = true;
			break;
		}
		at += 46 + nameLength + le16(e + 30) + le16(e + 32);
	}
	free(dir);
	if (!found || (method != 0 && method != 8)) {
		logLine("zip: no game in it%s", found ? " that isn't stored or deflated" : "");
		return NULL;
	}
	uint8_t header[30];
	fseek(f, local, SEEK_SET);
	if (fread(header, 1, 30, f) != 30 || le32(header) != 0x04034b50) {
		return NULL;
	}
	fseek(f, local + 30 + le16(header + 26) + le16(header + 28), SEEK_SET);
	uint8_t* out = gameBuffer(innerName, uncompressed, offered);
	if (!out) {
		return NULL;
	}
	bool ok;
	if (method == 0) {
		ok = fread(out, 1, uncompressed, f) == uncompressed;
	} else {
		z_stream z = { 0 };
		ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
		enum { CHUNK = 64 << 10 };
		uint8_t* in = ok ? malloc(CHUNK) : NULL;
		ok = ok && in;
		z.next_out = out;
		z.avail_out = uncompressed;
		uint32_t left = compressed;
		int status = Z_OK;
		while (ok && status != Z_STREAM_END) {
			if (!z.avail_in) {
				uint32_t take = left < CHUNK ? left : CHUNK;
				if (!take || fread(in, 1, take, f) != take) {
					ok = false;
					break;
				}
				left -= take;
				z.next_in = in;
				z.avail_in = take;
			}
			status = inflate(&z, Z_NO_FLUSH);
			ok = status == Z_OK || status == Z_STREAM_END;
		}
		ok = ok && z.total_out == uncompressed;
		inflateEnd(&z);
		free(in);
	}
	if (!ok) {
		logLine("zip: %s didn't inflate", innerName);
		free(out);
		*offered = false;
		return NULL;
	}
	*size = uncompressed;
	return out;
}

static void* sevenZipLoad(const char* path, size_t* size, char* innerName, size_t innerSize, bool* offered) {
	*offered = false;
	struct string_list* entries = file_archive_get_file_list(path, "gb|gbc|gba|bin|agb");
	if (!entries || !entries->size) {
		string_list_free(entries);
		return NULL;
	}
	snprintf(innerName, innerSize, "%s", entries->elems[0].data);
	char inner[1100];
	snprintf(inner, sizeof(inner), "%s#%s", path, entries->elems[0].data);
	string_list_free(entries);
	void* data = NULL;
	int64_t length = 0;
	if (!file_archive_compressed_read(inner, &data, NULL, &length) || length <= 0) {
		free(data);
		return NULL;
	}
	// A GBA game into a cartridge-sized buffer, so the core can take it over (it would copy it
	// anyway: this way the peak is the same and nothing is kept twice afterwards).
	if (isGbaName(innerName) && length <= SIZE_ROM) {
		uint8_t* buffer = calloc(1, SIZE_ROM);
		if (buffer) {
			memcpy(buffer, data, (size_t) length);
			free(data);
			data = buffer;
			*offered = true;
		}
	}
	*size = (size_t) length;
	return data;
}

void* romLoad(const char* path, size_t* size, char* innerName, size_t innerSize, bool* offered) {
	*offered = false;
	const char* ext = extensionOf(path);
	if (!strcasecmp(ext, "7z")) {
		return sevenZipLoad(path, size, innerName, innerSize, offered);
	}
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	void* data = NULL;
	if (!strcasecmp(ext, "zip")) {
		data = zipLoad(f, size, innerName, innerSize, offered);
	} else {
		const char* slash = strrchr(path, '/');
		snprintf(innerName, innerSize, "%s", slash ? slash + 1 : path);
		fseek(f, 0, SEEK_END);
		long length = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (length > 0) {
			data = gameBuffer(innerName, (size_t) length, offered);
			if (data && fread(data, 1, (size_t) length, f) != (size_t) length) {
				free(data);
				data = NULL;
				*offered = false;
			}
			*size = (size_t) length;
		}
	}
	fclose(f);
	return data;
}
