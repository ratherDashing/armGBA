/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Import Cheats from RetroArch, for Future Console Design's Xploder Advance and CodeBreaker, whose
// programs keep their codes in a database in their flash chip (nothing about it is published; this
// is what their programs do with it):
// - It runs from resource 4 of the table the header points to (at C4) up to resource 6, where the
//   codes compiled for a game start. For each game: a count of its items, its name (ended by 0),
//   then the items; for each item: a count of its codes, its name, then the codes, 6 bytes each (the
//   first 8 digits as typed, a little-endian word, then the last 4, a little-endian halfword). A
//   count of 0 and an empty name end it.
// - A game's first code, its master code's first line, is a seed (type 9): the game's other codes
//   are kept as typed, encrypted with a key it gives, and decrypted only when the game starts. Its
//   master code also has a line of type 0 holding the CRC-16 of the game's first 64 KiB, which the
//   program finds the game on top by.
// - Their programs warn when the database outgrows its room but write it anyway, over the compiled
//   codes, so the import keeps within it.

enum {
	RESOURCES = 0xC4,
	DATABASE = 4,
	DATABASE_END = 6,
	CODE = 6,
	NAME = 24,
	MOST = 255,
};

static uint32_t read32(const uint8_t* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

// The CodeBreaker's cipher, as its program decrypts (the encryption is its inverse): a 48-bit
// permutation, masks from a linear congruential generator, and two passes chaining the bytes.
struct Key {
	uint8_t perm[48];
	uint32_t t230;
	uint32_t t234;
	uint32_t t238;
	uint32_t t23c;
	uint16_t key;
};

struct Rng {
	uint32_t s;
};

static uint32_t lcg(struct Rng* r) {
	r->s = r->s * 0x41C64E6Du + 0x3039;
	return r->s;
}

static uint32_t generate(struct Rng* r) {
	uint32_t const s1 = lcg(r);
	uint32_t const s2 = lcg(r);
	uint32_t const s3 = lcg(r);
	return (s1 >> 16) << 30 | ((s2 >> 16) & 0x7FFF) << 15 | ((s3 >> 16) & 0x7FFF);
}

static void keyFor(uint32_t w0, uint16_t v, struct Key* k) {
	struct Rng r = { (uint32_t) (v & 0xFF) ^ 0x1111 };
	for (int i = 0; i < 48; ++i) {
		k->perm[i] = (uint8_t) i;
	}
	for (int i = 0; i < 0x50; ++i) {
		uint32_t const a = generate(&r) % 48;
		uint32_t const b = generate(&r) % 48;
		uint8_t const t = k->perm[a];
		k->perm[a] = k->perm[b];
		k->perm[b] = t;
	}
	r.s = 0x4EFAD1C3;
	for (uint32_t i = 0; i < ((w0 >> 24) & 0xF); ++i) {
		r.s = generate(&r);
	}
	k->t238 = generate(&r);
	k->t23c = generate(&r);
	unsigned const high = v >> 8 & 0xFF;
	r.s = high ^ 0xF254;
	for (unsigned i = 0; i < high; ++i) {
		r.s = generate(&r);
	}
	k->t230 = generate(&r);
	k->t234 = generate(&r);
	k->key = (uint16_t) w0;
}

static void toBytes(uint32_t w0, uint16_t v, uint8_t b[6]) {
	b[0] = (uint8_t) (w0 >> 24);
	b[1] = (uint8_t) (w0 >> 16);
	b[2] = (uint8_t) (w0 >> 8);
	b[3] = (uint8_t) w0;
	b[4] = (uint8_t) (v >> 8);
	b[5] = (uint8_t) v;
}

static void fromBytes(const uint8_t b[6], uint32_t* w0, uint16_t* v) {
	*w0 = (uint32_t) b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
	*v = (uint16_t) (b[4] << 8 | b[5]);
}

static void swapBits(uint8_t b[6], unsigned i, unsigned j) {
	unsigned const bi = b[i >> 3] >> (i & 7) & 1;
	unsigned const bj = b[j >> 3] >> (j & 7) & 1;
	b[i >> 3] = (uint8_t) ((b[i >> 3] & ~(1u << (i & 7))) | bj << (i & 7));
	b[j >> 3] = (uint8_t) ((b[j >> 3] & ~(1u << (j & 7))) | bi << (j & 7));
}

static void decrypt(const struct Key* k, uint32_t* w0, uint16_t* v) {
	uint8_t b[6];
	toBytes(*w0, *v, b);
	for (int i = 47; i >= 0; --i) {
		swapBits(b, (unsigned) i, k->perm[i]);
	}
	fromBytes(b, w0, v);
	*w0 ^= k->t230;
	*v ^= (uint16_t) k->t234;
	toBytes(*w0, *v, b);
	uint8_t const kh = k->key >> 8;
	uint8_t const kl = (uint8_t) k->key;
	for (int i = 0; i < 5; ++i) {
		b[i] ^= b[i + 1] ^ kh;
	}
	b[5] ^= kh;
	for (int i = 5; i >= 0; --i) {
		b[i] ^= (i ? b[i - 1] : 0) ^ kl;
	}
	fromBytes(b, w0, v);
	*w0 ^= k->t238;
	*v ^= (uint16_t) k->t23c;
}

static void encrypt(const struct Key* k, uint32_t* w0, uint16_t* v) {
	*w0 ^= k->t238;
	*v ^= (uint16_t) k->t23c;
	uint8_t b[6];
	toBytes(*w0, *v, b);
	uint8_t const kh = k->key >> 8;
	uint8_t const kl = (uint8_t) k->key;
	b[0] ^= kl;
	for (int i = 1; i < 6; ++i) {
		b[i] ^= b[i - 1] ^ kl;
	}
	b[5] ^= kh;
	for (int i = 4; i >= 0; --i) {
		b[i] ^= b[i + 1] ^ kh;
	}
	fromBytes(b, w0, v);
	*w0 ^= k->t230;
	*v ^= (uint16_t) k->t234;
	toBytes(*w0, *v, b);
	for (int i = 0; i < 48; ++i) {
		swapBits(b, (unsigned) i, k->perm[i]);
	}
	fromBytes(b, w0, v);
}

static void loadCode(const uint8_t* p, uint32_t* w0, uint16_t* v) {
	*w0 = read32(p);
	*v = (uint16_t) (p[4] | p[5] << 8);
}

static void storeCode(uint8_t* p, uint32_t w0, uint16_t v) {
	p[0] = (uint8_t) w0;
	p[1] = (uint8_t) (w0 >> 8);
	p[2] = (uint8_t) (w0 >> 16);
	p[3] = (uint8_t) (w0 >> 24);
	p[4] = (uint8_t) v;
	p[5] = (uint8_t) (v >> 8);
}

// The CRC-16 the programs know a game by: CCITT, from FFFF, of its first 64 KiB.
static uint16_t crc16(const uint8_t* data, size_t size) {
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < size; ++i) {
		crc ^= (uint16_t) (data[i] << 8);
		for (int b = 0; b < 8; ++b) {
			crc = crc & 0x8000 ? (uint16_t) (crc << 1 ^ 0x1021) : (uint16_t) (crc << 1);
		}
	}
	return crc;
}

int gbaGameId(const uint8_t* game, size_t size) {
	return game && size ? crc16(game, size < 0x10000 ? size : 0x10000) : -1;
}

// The end of the string at at, before end, or 0.
static size_t skipName(const uint8_t* db, size_t at, size_t end) {
	while (at < end && db[at]) {
		++at;
	}
	return at < end ? at + 1 : 0;
}

// A cheat's codes from RetroArch's database: pairs of 8 and 4 digits joined by + (or a space). As the
// CodeBreaker's own engine reads them, a line after a seed is decrypted with its key first, and a seed
// (type 9) then sets the key for the lines after it. A cheat with a master code (types 0 and 1),
// whose seed it may hold, isn't taken (*master then set), nor anything else not a code. The lines
// after a slide (type 4: one) or a fill list (type 5: one for each three halfwords of its count) are
// its data, as the engine reads them, not codes.
static size_t plainCodes(const char* text, struct Key* seed, bool* seeded, uint32_t (*w0s)[2], size_t most, bool* master) {
	char copy[sizeof(((struct ChtCheat*) 0)->code)];
	snprintf(copy, sizeof(copy), "%s", text);
	size_t n = 0;
	*master = false;
	char* first = NULL;
	unsigned data = 0;
	for (char* part = strtok(copy, "+ "); part; part = strtok(NULL, "+ ")) {
		if (!first) {
			first = part;
			continue;
		}
		unsigned long w0;
		unsigned long v;
		char* end;
		if (strlen(first) != 8 || strlen(part) != 4 || (w0 = strtoul(first, &end, 16), *end) || (v = strtoul(part, &end, 16), *end)) {
			return 0;
		}
		first = NULL;
		uint32_t word = (uint32_t) w0;
		uint16_t value = (uint16_t) v;
		if (*seeded) {
			decrypt(seed, &word, &value);
		}
		if (data) {
			--data;
			if (n >= most) {
				return 0;
			}
			w0s[n][0] = word;
			w0s[n][1] = value;
			++n;
			continue;
		}
		uint32_t const type = word >> 28;
		if (type == 9) {
			keyFor(word, value, seed);
			*seeded = true;
			continue;
		}
		if (type == 0 || type == 1) {
			*master = true;
			continue;
		}
		if (n >= most) {
			return 0;
		}
		w0s[n][0] = word;
		w0s[n][1] = value;
		++n;
		data = type == 4 ? 1 : type == 5 ? (value + 2u) / 3u : 0;
	}
	return first || *master || data ? 0 : n;
}

int fcdImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
              size_t* already, size_t* left, size_t* unfit) {
	*already = *left = *unfit = 0;
	if (size < RESOURCES + 4 || gameSize < 0x10000) {
		return -1;
	}
	uint32_t const table = read32(&flash[RESOURCES]) & 0x1FFFFFF;
	if (table + 4 * (DATABASE_END + 1) > size) {
		return -1;
	}
	size_t const start = read32(&flash[table + 4 * DATABASE]) & 0x1FFFFFF;
	size_t const room = read32(&flash[table + 4 * DATABASE_END]) & 0x1FFFFFF;
	if (start >= room || room > size) {
		return -1;
	}
	const uint8_t* const db = &flash[start];
	size_t const end = room - start;
	uint16_t const crc = crc16(game, 0x10000);
	// The game whose master code has the CRC of the game on top: where its items end, its count, its key
	size_t at = 0;
	size_t ours = SIZE_MAX;
	size_t oursEnd = 0;
	size_t oursFirstItem = 0;
	struct Key key;
	bool keyed = false;
	while (at + 1 < end && (db[at] || db[at + 1])) {
		size_t const gameAt = at;
		unsigned const items = db[at];
		if (!(at = skipName(db, at + 1, end))) {
			return -1;
		}
		size_t const firstItem = at;
		struct Key gameKey;
		bool gameKeyed = false;
		bool matches = false;
		bool first = true;
		for (unsigned i = 0; i < items; ++i) {
			if (at >= end) {
				return -1;
			}
			unsigned const codes = db[at];
			if (!(at = skipName(db, at + 1, end)) || at + codes * CODE > end) {
				return -1;
			}
			for (unsigned c = 0; c < codes; ++c, at += CODE) {
				uint32_t w0;
				uint16_t v;
				loadCode(&db[at], &w0, &v);
				if (first && w0 >> 28 == 9) {
					keyFor(w0, v, &gameKey);
					gameKeyed = true;
				} else {
					if (gameKeyed) {
						decrypt(&gameKey, &w0, &v);
					}
					matches |= w0 >> 28 == 0 && (uint16_t) w0 == crc;
				}
				first = false;
			}
		}
		if (matches && ours == SIZE_MAX) {
			ours = gameAt;
			oursEnd = at;
			oursFirstItem = firstItem;
			key = gameKey;
			keyed = gameKeyed;
		}
	}
	if (at + 1 >= end) {
		return -1;
	}
	size_t const dbEnd = at + 2;
	if (ours == SIZE_MAX) {
		*left = count;
		return -3;
	}
	uint8_t* const out = malloc(end);
	if (!out) {
		return -1;
	}
	memcpy(out, db, oursEnd);
	size_t used = oursEnd;
	int added = 0;
	struct Key seed;
	bool seeded = false;
	for (size_t i = 0; i < count; ++i) {
		// A seed holds for the cheats after it in its own file
		seeded &= i && cheats[i].file == cheats[i - 1].file;
		uint32_t codes[64][2];
		bool master;
		size_t const n = plainCodes(cheats[i].code, &seed, &seeded, codes, 64, &master);
		if (!n) {
			*unfit += !master;
			continue;
		}
		// Already the game's, if each of its codes is, in any item
		size_t have = 0;
		for (size_t c = 0; c < n; ++c) {
			bool found = false;
			for (size_t p = oursFirstItem; p < used && !found;) {
				unsigned const lines = out[p];
				p = skipName(out, p + 1, used);
				for (unsigned l = 0; l < lines && !found; ++l, p += CODE) {
					uint32_t w0;
					uint16_t v;
					loadCode(&out[p], &w0, &v);
					// (The seed decrypts to something no code is.)
					if (keyed) {
						decrypt(&key, &w0, &v);
					}
					found = w0 == codes[c][0] && v == codes[c][1];
				}
			}
			have += found;
		}
		if (have == n) {
			++*already;
			continue;
		}
		char name[NAME + 1];
		size_t length = 0;
		for (const char* s = cheats[i].description; *s && length < NAME; ++s) {
			name[length++] = *s >= 0x20 && *s < 0x7F && *s != '~' ? *s : '?';
		}
		while (length && name[length - 1] == ' ') {
			--length;
		}
		name[length] = '\0';
		size_t const need = 2 + length + n * CODE;
		if (out[ours] == MOST || used + need + (dbEnd - oursEnd) > end) {
			++*left;
			continue;
		}
		out[used++] = (uint8_t) n;
		memcpy(&out[used], name, length + 1);
		used += length + 1;
		for (size_t c = 0; c < n; ++c, used += CODE) {
			uint32_t w0 = codes[c][0];
			uint16_t v = (uint16_t) codes[c][1];
			if (keyed) {
				encrypt(&key, &w0, &v);
			}
			storeCode(&out[used], w0, v);
		}
		++out[ours];
		++added;
	}
	memcpy(&out[used], &db[oursEnd], dbEnd - oursEnd);
	used += dbEnd - oursEnd;
	memset(&out[used], 0xFF, end - used);
	memcpy(&flash[start], out, end);
	free(out);
	return added;
}

int fcdClear(uint8_t* flash, size_t size, bool keepMasters) {
	if (size < RESOURCES + 4) {
		return -1;
	}
	uint32_t const table = read32(&flash[RESOURCES]) & 0x1FFFFFF;
	if (table + 4 * (DATABASE_END + 1) > size) {
		return -1;
	}
	size_t const start = read32(&flash[table + 4 * DATABASE]) & 0x1FFFFFF;
	size_t const room = read32(&flash[table + 4 * DATABASE_END]) & 0x1FFFFFF;
	if (start >= room || room > size) {
		return -1;
	}
	const uint8_t* const db = &flash[start];
	size_t const end = room - start;
	uint8_t* const out = malloc(end);
	if (!out) {
		return -1;
	}
	size_t used = 0;
	size_t at = 0;
	while (at + 1 < end && (db[at] || db[at + 1])) {
		size_t const gameAt = at;
		unsigned const items = db[at];
		if (!(at = skipName(db, at + 1, end))) {
			free(out);
			return -1;
		}
		size_t firstEnd = at;
		for (unsigned i = 0; i < items; ++i) {
			unsigned const codes = at < end ? db[at] : 0;
			if (at >= end || !(at = skipName(db, at + 1, end)) || at + codes * CODE > end) {
				free(out);
				return -1;
			}
			at += codes * CODE;
			if (!i) {
				firstEnd = at;
			}
		}
		// The game with its first item, its master code (whose seed and checksum find the game)
		if (keepMasters) {
			out[used] = (uint8_t) (items ? 1 : 0);
			memcpy(&out[used + 1], &db[gameAt + 1], firstEnd - gameAt - 1);
			used += firstEnd - gameAt;
		}
	}
	if (at + 1 >= end) {
		free(out);
		return -1;
	}
	// The end: no items and an empty name
	out[used++] = 0;
	out[used++] = 0;
	memset(&out[used], 0xFF, end - used);
	memcpy(&flash[start], out, end);
	free(out);
	return 0;
}

// A check of the cipher against the CodeBreaker's own (Castlevania: Circle of the Moon, seed
// 90173540 1263: 3202566A 0001 is typed C8AD366D 81F1).
bool fcdCipherWorks(void) {
	struct Key k;
	keyFor(0x90173540, 0x1263, &k);
	uint32_t w0 = 0x3202566A;
	uint16_t v = 0x0001;
	encrypt(&k, &w0, &v);
	bool const encrypts = w0 == 0xC8AD366D && v == 0x81F1;
	decrypt(&k, &w0, &v);
	return encrypts && w0 == 0x3202566A && v == 0x0001;
}

// Datel's Action Replay v3 and GBX for the Game Boy Advance keep their list as LibreShark's
// sharkdumps documents it (imhex-gba-gs-datel-rom-pattern): "GBA_" at 21000, then at 21040 a count
// of games (32 bits), and for each its name (20, padded with spaces) and a count of its cheats (32
// bits); for each cheat a word with the count of its words in the low byte and flags in the top one
// (80 while it's on, C0 for a master code), its name (20), then the words: GameShark codes, two words
// each, encrypted with the GameShark's key (TEA). A game's master code has a line xxxxxxxx 001DC0DE
// whose first word is the 4-letter code in the game's header (at AC), which the Action Replay finds
// the game on top by. CodeBreaker writes of 8 and 16 bits go in as the GameShark's (types 0 and 1).
// The Action Replay v3.3 keeps the same list with Action Replay v3 codes instead, as the MAX's are:
// encrypted with the Action Replay's own key, writes of 8 and 16 bits as types 0 and 2 to a packed
// address. Which a list holds, its master codes' ID lines tell.
// After the last game the program leaves an empty one (a name left erased and no cheats), which
// stays after the list; the header's word at 21010 (4000) is taken for the most the list may take.
enum {
	AR_MARK = 0x21000,
	AR_GAMES = 0x21040,
	AR_NAME = 20,
	AR_ID = 0x001DC0DE,
	AR_MOST_WORDS = 0xFF,
	AR_SIZE_AT = 0x21010,
	AR_END_MARK = AR_NAME + 4,
};

static const uint32_t gameSharkSeeds[4] = { 0x09F4FBBD, 0x9681884A, 0x352027E9, 0xF3DEE5A7 };

// The Action Replay v3's own key (the MAX's codes)
static const uint32_t actionReplaySeeds[4] = { 0x7AA9648F, 0x7FAE6994, 0xC0EFAAD5, 0x42712C57 };

static void teaDecryptWith(const uint32_t* seeds, uint32_t* op1, uint32_t* op2) {
	uint32_t sum = 0xC6EF3720;
	for (int i = 0; i < 32; ++i) {
		*op2 -= ((*op1 << 4) + seeds[2]) ^ (*op1 + sum) ^ ((*op1 >> 5) + seeds[3]);
		*op1 -= ((*op2 << 4) + seeds[0]) ^ (*op2 + sum) ^ ((*op2 >> 5) + seeds[1]);
		sum -= 0x9E3779B9;
	}
}

static void teaEncryptWith(const uint32_t* seeds, uint32_t* op1, uint32_t* op2) {
	uint32_t sum = 0;
	for (int i = 0; i < 32; ++i) {
		sum += 0x9E3779B9;
		*op1 += ((*op2 << 4) + seeds[0]) ^ (*op2 + sum) ^ ((*op2 >> 5) + seeds[1]);
		*op2 += ((*op1 << 4) + seeds[2]) ^ (*op1 + sum) ^ ((*op1 >> 5) + seeds[3]);
	}
}

static void teaDecrypt(uint32_t* op1, uint32_t* op2) {
	teaDecryptWith(gameSharkSeeds, op1, op2);
}

static void teaEncrypt(uint32_t* op1, uint32_t* op2) {
	teaEncryptWith(gameSharkSeeds, op1, op2);
}

static void write32(uint8_t* p, uint32_t value) {
	p[0] = (uint8_t) value;
	p[1] = (uint8_t) (value >> 8);
	p[2] = (uint8_t) (value >> 16);
	p[3] = (uint8_t) (value >> 24);
}

int arGbaImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
                size_t* already, size_t* left, size_t* unfit) {
	*already = *left = *unfit = 0;
	if (size < AR_GAMES + 4 || gameSize < 0xB0) {
		return -1;
	}
	if (memcmp(&flash[AR_MARK], "GBA_", 4)) {
		// The Action Replay MAX's, kept another way
		return -4;
	}
	uint32_t const id = read32(&game[0xAC]);
	uint32_t const games = read32(&flash[AR_GAMES]);
	if (games > 10000) {
		return -1;
	}
	// The game with the ID of the game on top: where its cheats' count is and where they end
	size_t at = AR_GAMES + 4;
	size_t countAt = 0;
	size_t ours = 0;
	size_t oursFirst = 0;
	// The key the game's master code reads with: the Action Replay v3's on the v3.3
	const uint32_t* keys = gameSharkSeeds;
	for (uint32_t g = 0; g < games; ++g) {
		if (at + AR_NAME + 4 > size) {
			return -1;
		}
		size_t const cheatsAt = at + AR_NAME;
		uint32_t const cheatsThere = read32(&flash[cheatsAt]);
		at = cheatsAt + 4;
		size_t const first = at;
		bool matches = false;
		const uint32_t* matchedKeys = gameSharkSeeds;
		for (uint32_t c = 0; c < cheatsThere; ++c) {
			if (at + 4 + AR_NAME > size) {
				return -1;
			}
			size_t const words = read32(&flash[at]) & 0xFFFFFF;
			if (words > (size - at - 4 - AR_NAME) / 4) {
				return -1;
			}
			for (size_t w = 0; w + 1 < words; w += 2) {
				for (int k = 0; k < 2 && !matches; ++k) {
					const uint32_t* const seeds = k ? actionReplaySeeds : gameSharkSeeds;
					uint32_t op1 = read32(&flash[at + 4 + AR_NAME + w * 4]);
					uint32_t op2 = read32(&flash[at + 4 + AR_NAME + w * 4 + 4]);
					teaDecryptWith(seeds, &op1, &op2);
					if (op2 == AR_ID && op1 == id) {
						matches = true;
						matchedKeys = seeds;
					}
				}
			}
			at += 4 + AR_NAME + words * 4;
		}
		if (matches && !countAt) {
			countAt = cheatsAt;
			ours = at;
			oursFirst = first;
			keys = matchedKeys;
		}
	}
	if (!countAt) {
		*left = count;
		return -3;
	}
	// The empty game after the list goes with it
	size_t listEnd = at;
	bool marked = listEnd + AR_END_MARK <= size && !read32(&flash[listEnd + AR_NAME]);
	for (size_t i = 0; marked && i < AR_NAME; ++i) {
		marked = flash[listEnd + i] == 0xFF;
	}
	if (marked) {
		listEnd += AR_END_MARK;
	}
	// Room: the erased flash after it, up to the most the list may take
	// (on the v3.3 that word is something else, the list already past it: then the chip's end)
	uint32_t const most = read32(&flash[AR_SIZE_AT]);
	size_t const limit = most >= 0x1000 && AR_MARK + most <= size && AR_MARK + most >= listEnd ? AR_MARK + most : size;
	size_t room = listEnd;
	while (room < limit && flash[room] == 0xFF) {
		++room;
	}
	uint8_t* const out = malloc(size - AR_GAMES);
	if (!out) {
		return -1;
	}
	size_t used = 0;
	memcpy(out, &flash[AR_GAMES], ours - AR_GAMES);
	used = ours - AR_GAMES;
	size_t const outCount = countAt - AR_GAMES;
	int added = 0;
	struct Key seed;
	bool seeded = false;
	for (size_t i = 0; i < count; ++i) {
		// A seed holds for the cheats after it in its own file
		seeded &= i && cheats[i].file == cheats[i - 1].file;
		uint32_t lines[64][2];
		bool master;
		size_t const n = plainCodes(cheats[i].code, &seed, &seeded, lines, 64, &master);
		if (!n) {
			*unfit += !master;
			continue;
		}
		// As the list's codes: 8 and 16-bit writes only, as the GameShark's or the Action Replay v3's (to
		// an address it can hold)
		bool const actionReplay = keys == actionReplaySeeds;
		uint32_t codes[64][2];
		bool converts = true;
		for (size_t c = 0; c < n && converts; ++c) {
			uint32_t const type = lines[c][0] >> 28;
			uint32_t const address = lines[c][0] & 0x0FFFFFFF;
			converts = (type == 3 || type == 8) && !(actionReplay && (address & 0x00F00000));
			uint32_t const packed = (address & 0xFFFFF) | ((address >> 4) & 0x00F00000);
			codes[c][0] = actionReplay ? (type == 3 ? 0x00000000u : 0x02000000u) | packed : (type == 3 ? 0x00000000u : 0x10000000u) | address;
			codes[c][1] = type == 3 ? (lines[c][1] & 0xFF) : (lines[c][1] & 0xFFFF);
		}
		if (!converts) {
			++*unfit;
			continue;
		}
		// Already the game's, if each of its codes is
		size_t have = 0;
		for (size_t c = 0; c < n; ++c) {
			bool found = false;
			for (size_t p = oursFirst - AR_GAMES; p < used && !found;) {
				size_t const words = read32(&out[p]) & 0xFFFFFF;
				for (size_t w = 0; w + 1 < words && !found; w += 2) {
					uint32_t op1 = read32(&out[p + 4 + AR_NAME + w * 4]);
					uint32_t op2 = read32(&out[p + 4 + AR_NAME + w * 4 + 4]);
					teaDecryptWith(keys, &op1, &op2);
					found = op1 == codes[c][0] && op2 == codes[c][1];
				}
				p += 4 + AR_NAME + words * 4;
			}
			have += found;
		}
		if (have == n) {
			++*already;
			continue;
		}
		size_t const need = 4 + AR_NAME + n * 8;
		if (n * 2 > AR_MOST_WORDS || AR_GAMES + used + need + (listEnd - ours) > room) {
			++*left;
			continue;
		}
		write32(&out[used], (uint32_t) (n * 2));
		size_t length = 0;
		for (const char* s = cheats[i].description; *s && length < AR_NAME; ++s) {
			out[used + 4 + length++] = (uint8_t) (*s >= 0x20 && *s < 0x7F ? *s : '?');
		}
		memset(&out[used + 4 + length], ' ', AR_NAME - length);
		for (size_t c = 0; c < n; ++c) {
			uint32_t op1 = codes[c][0];
			uint32_t op2 = codes[c][1];
			teaEncryptWith(keys, &op1, &op2);
			write32(&out[used + 4 + AR_NAME + c * 8], op1);
			write32(&out[used + 4 + AR_NAME + c * 8 + 4], op2);
		}
		used += need;
		write32(&out[outCount], read32(&out[outCount]) + 1);
		++added;
	}
	memcpy(&out[used], &flash[ours], listEnd - ours);
	used += listEnd - ours;
	memcpy(&flash[AR_GAMES], out, used);
	free(out);
	return added;
}

int arGbaClear(uint8_t* flash, size_t size, bool keepMasters) {
	if (size < AR_GAMES + 4) {
		return -1;
	}
	if (memcmp(&flash[AR_MARK], "GBA_", 4)) {
		return -4;
	}
	uint32_t const games = read32(&flash[AR_GAMES]);
	if (games > 10000) {
		return -1;
	}
	uint8_t* const out = malloc(size - AR_GAMES);
	if (!out) {
		return -1;
	}
	size_t used = 4;
	size_t at = AR_GAMES + 4;
	for (uint32_t g = 0; g < games; ++g) {
		if (at + AR_NAME + 4 > size) {
			free(out);
			return -1;
		}
		size_t const named = used;
		if (keepMasters) {
			memcpy(&out[used], &flash[at], AR_NAME);
			used += AR_NAME + 4;
		}
		uint32_t const cheatsThere = read32(&flash[at + AR_NAME]);
		at += AR_NAME + 4;
		uint32_t kept = 0;
		for (uint32_t c = 0; c < cheatsThere; ++c) {
			if (at + 4 + AR_NAME > size) {
				free(out);
				return -1;
			}
			uint32_t const word = read32(&flash[at]);
			size_t const words = word & 0xFFFFFF;
			if (words > (size - at - 4 - AR_NAME) / 4) {
				free(out);
				return -1;
			}
			size_t const length = 4 + AR_NAME + words * 4;
			// A master code (flag 40, with 80 for on), which the game is found by
			if (keepMasters && (word >> 24 & 0x40)) {
				memcpy(&out[used], &flash[at], length);
				used += length;
				++kept;
			}
			at += length;
		}
		if (keepMasters) {
			write32(&out[named + AR_NAME], kept);
		}
	}
	// The empty game after the list goes too, and comes back after the new one
	size_t listEnd = at;
	bool marked = listEnd + AR_END_MARK <= size && !read32(&flash[listEnd + AR_NAME]);
	for (size_t i = 0; marked && i < AR_NAME; ++i) {
		marked = flash[listEnd + i] == 0xFF;
	}
	if (marked) {
		listEnd += AR_END_MARK;
	}
	write32(out, keepMasters ? games : 0);
	memset(&out[used], 0xFF, AR_NAME);
	write32(&out[used + AR_NAME], 0);
	used += AR_END_MARK;
	size_t const was = listEnd - AR_GAMES;
	if (used < was) {
		memset(&out[used], 0xFF, was - used);
	}
	memcpy(&flash[AR_GAMES], out, used > was ? used : was);
	free(out);
	return 0;
}

// The GameShark key's encryption against its decryption.
bool arGbaCipherWorks(void) {
	uint32_t op1 = 0x03004538;
	uint32_t op2 = 0x000000FF;
	teaEncrypt(&op1, &op2);
	uint32_t e1 = op1;
	uint32_t e2 = op2;
	teaDecrypt(&op1, &op2);
	// Army Men Advance's "Enemy has bad aim", as the Action Replay GBX keeps it
	return e1 == 0xF8E643BF && e2 == 0x724A7B8B && op1 == 0x03004538 && op2 == 0x000000FF;
}

// Datel's Action Replay MAX keeps its code lists as files in a small file system in its flash chip
// (nothing about it is published; this is what its program does with it):
// - Two copies of a cluster table, at 30000 and 32000 (8 KiB each): two checksums at 0 and 2, a
//   generation at 4, the count of free clusters at 6 and the last cluster given out at 8, and a
//   big-endian entry for each cluster at twice its number (0 free, FFFF the last of a file, else the
//   next). The clusters are 0x80 bytes from 30000, 200 to 9FF the ones files have.
// - Two copies of a directory, at 34000 and 3A000 (0x6000 bytes each): 32 bytes a file (a code word,
//   for a code list the 4 letters in the game's header at AC; a name of 23; flags, 10 for a code
//   list; its first cluster and its count of clusters, big-endian), then a generation at 5FFA and
//   two checksums.
// - A checksum is of the big-endian halfwords: their sum, and the sum of their complements (FFFF
//   written as 0). The program takes the copy with the higher generation (the second on a tie),
//   and a change goes in the other copy with a generation above all four.
// - A code list: a count of its cheats, then for each a word with the count of its words and flags
//   above (80000000 on, 40000000 a master code, 20000000 a heading), a name of 20, then the words,
//   the Action Replay v3's codes encrypted with its key; zeros to the end of the last cluster.
// CodeBreaker writes of 8 and 16 bits go in as the Action Replay v3's.
enum {
	MAX_TABLES = 0x30000,
	MAX_TABLE = 0x2000,
	MAX_DIRECTORIES = 0x34000,
	MAX_DIRECTORY = 0x6000,
	MAX_GENERATION = 0x5FFA,
	MAX_SUMS = 0x5FFC,
	MAX_ENTRY = 32,
	MAX_ENTRIES = MAX_GENERATION / MAX_ENTRY,
	MAX_LIST = 0x10,
	MAX_CLUSTER = 0x80,
	MAX_FIRST = 0x200,
	MAX_END = 0xA00,
	MAX_TABLE_SUMMED = 0x110,
	MAX_ON = 0x80000000u,
};

static unsigned read16Big(const uint8_t* p) {
	return p[0] << 8 | p[1];
}

static void write16Big(uint8_t* p, unsigned value) {
	p[0] = (uint8_t) (value >> 8);
	p[1] = (uint8_t) value;
}

static void maxSums(const uint8_t* data, size_t size, uint8_t* out) {
	unsigned s1 = 0;
	unsigned s2 = 0;
	for (size_t i = 0; i + 1 < size; i += 2) {
		unsigned const h = read16Big(&data[i]);
		s1 = (s1 + h) & 0xFFFF;
		s2 = (s2 + (h ^ 0xFFFF)) & 0xFFFF;
	}
	write16Big(&out[0], s1 == 0xFFFF ? 0 : s1);
	write16Big(&out[2], s2 == 0xFFFF ? 0 : s2);
}

static bool maxSumsHold(const uint8_t* data, size_t size, const uint8_t* stored) {
	uint8_t sums[4];
	maxSums(data, size, sums);
	return !memcmp(sums, stored, 4);
}

static unsigned maxLink(const uint8_t* table, unsigned cluster) {
	return read16Big(&table[2 * cluster]);
}

int maxImport(uint8_t* flash, size_t size, const uint8_t* game, size_t gameSize, const struct ChtCheat* cheats, size_t count,
              size_t* already, size_t* left, size_t* unfit) {
	*already = *left = *unfit = 0;
	// Up to the last cluster
	if (size < (size_t) MAX_TABLES + (size_t) MAX_END * MAX_CLUSTER || gameSize < 0xB0) {
		return -1;
	}
	// The copies in use, and the highest generation of those whole (an erased copy reads FFFF)
	uint8_t* const directories[2] = { &flash[MAX_DIRECTORIES], &flash[MAX_DIRECTORIES + MAX_DIRECTORY] };
	uint8_t* const tables[2] = { &flash[MAX_TABLES], &flash[MAX_TABLES + MAX_TABLE] };
	unsigned const dirGen[2] = { read16Big(&directories[0][MAX_GENERATION]), read16Big(&directories[1][MAX_GENERATION]) };
	unsigned const tableGen[2] = { read16Big(&tables[0][4]), read16Big(&tables[1][4]) };
	int dirNow = dirGen[0] > dirGen[1] ? 0 : 1;
	if (!maxSumsHold(directories[dirNow], MAX_SUMS, &directories[dirNow][MAX_SUMS])) {
		dirNow ^= 1;
		if (!maxSumsHold(directories[dirNow], MAX_SUMS, &directories[dirNow][MAX_SUMS])) {
			return -1;
		}
	}
	int tableNow = tableGen[0] > tableGen[1] ? 0 : 1;
	if (!maxSumsHold(&tables[tableNow][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[tableNow][0])) {
		tableNow ^= 1;
		if (!maxSumsHold(&tables[tableNow][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[tableNow][0])) {
			return -1;
		}
	}
	unsigned generation = 0;
	for (int i = 0; i < 2; ++i) {
		if (maxSumsHold(directories[i], MAX_SUMS, &directories[i][MAX_SUMS]) && dirGen[i] > generation) {
			generation = dirGen[i];
		}
		if (maxSumsHold(&tables[i][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[i][0]) && tableGen[i] > generation) {
			generation = tableGen[i];
		}
	}
	if (generation + 2 >= 0xFFFF) {
		return -1;
	}
	// The game's code list: its 4 letters, as a code list
	const uint8_t* const directory = directories[dirNow];
	const uint8_t* const table = tables[tableNow];
	size_t slot = SIZE_MAX;
	for (size_t i = 0; i < MAX_ENTRIES; ++i) {
		const uint8_t* const entry = &directory[i * MAX_ENTRY];
		if (read16Big(&entry[0x1C]) != 0xFFFF && (entry[0x1B] & MAX_LIST) && !memcmp(entry, &game[0xAC], 4)) {
			slot = i;
			break;
		}
	}
	if (slot == SIZE_MAX) {
		*left = count;
		return -3;
	}
	const uint8_t* const entry = &directory[slot * MAX_ENTRY];
	unsigned const first = read16Big(&entry[0x1C]);
	unsigned const clusters = read16Big(&entry[0x1E]);
	if (!clusters || clusters > MAX_END - MAX_FIRST) {
		return -1;
	}
	// The file as it is, from its chain
	uint8_t* const file = malloc((size_t) (MAX_END - MAX_FIRST) * MAX_CLUSTER);
	if (!file) {
		return -1;
	}
	size_t fileSize = 0;
	unsigned cluster = first;
	// A chain that comes back on itself isn't a file
	uint8_t seen[(MAX_END + 7) / 8] = { 0 };
	for (unsigned i = 0; i < clusters; ++i) {
		if (cluster < MAX_FIRST || cluster >= MAX_END || seen[cluster / 8] & 1 << (cluster % 8)) {
			free(file);
			return -1;
		}
		seen[cluster / 8] |= (uint8_t) (1 << (cluster % 8));
		memcpy(&file[fileSize], &flash[MAX_TABLES + (size_t) cluster * MAX_CLUSTER], MAX_CLUSTER);
		fileSize += MAX_CLUSTER;
		cluster = maxLink(table, cluster);
	}
	// Its cheats: where they end, and that none changes the key (DEADFACE)
	unsigned const cheatsThere = (unsigned) (read32(file) & 0xFFFF);
	size_t at = 4;
	for (unsigned c = 0; c < cheatsThere; ++c) {
		if (at + 4 + AR_NAME > fileSize) {
			free(file);
			return -1;
		}
		size_t const words = read32(&file[at]) & 0xFFFF;
		if (at + 4 + AR_NAME + words * 4 > fileSize) {
			free(file);
			return -1;
		}
		for (size_t w = 0; w + 1 < words; w += 2) {
			uint32_t op1 = read32(&file[at + 4 + AR_NAME + w * 4]);
			uint32_t op2 = read32(&file[at + 4 + AR_NAME + w * 4 + 4]);
			teaDecryptWith(actionReplaySeeds, &op1, &op2);
			if (op1 == 0xDEADFACE) {
				free(file);
				return -1;
			}
		}
		at += 4 + AR_NAME + words * 4;
	}
	size_t const listEnd = at;
	// The cheats added after them, as long as the free clusters go
	unsigned const freeNow = read16Big(&table[6]);
	size_t room = (size_t) (freeNow + clusters) * MAX_CLUSTER;
	if (room > (size_t) (MAX_END - MAX_FIRST) * MAX_CLUSTER) {
		room = (size_t) (MAX_END - MAX_FIRST) * MAX_CLUSTER;
	}
	size_t used = listEnd;
	unsigned cheatsNow = cheatsThere;
	int added = 0;
	struct Key seed;
	bool seeded = false;
	for (size_t i = 0; i < count; ++i) {
		// A seed holds for the cheats after it in its own file
		seeded &= i && cheats[i].file == cheats[i - 1].file;
		uint32_t lines[64][2];
		bool master;
		size_t const n = plainCodes(cheats[i].code, &seed, &seeded, lines, 64, &master);
		if (!n) {
			*unfit += !master;
			continue;
		}
		// As the Action Replay v3's: writes of 8 and 16 bits, to an address it can hold
		uint32_t codes[64][2];
		bool converts = true;
		for (size_t c = 0; c < n && converts; ++c) {
			uint32_t const type = lines[c][0] >> 28;
			uint32_t const address = lines[c][0] & 0x0FFFFFFF;
			converts = (type == 3 || type == 8) && !(address & 0x00F00000);
			uint32_t const packed = (address & 0xFFFFF) | ((address >> 4) & 0x00F00000);
			codes[c][0] = (type == 3 ? 0x00000000u : 0x02000000u) | packed;
			codes[c][1] = type == 3 ? (lines[c][1] & 0xFF) : (lines[c][1] & 0xFFFF);
		}
		if (!converts) {
			++*unfit;
			continue;
		}
		size_t have = 0;
		for (size_t c = 0; c < n; ++c) {
			bool found = false;
			for (size_t p = 4; p < used && !found;) {
				size_t const words = read32(&file[p]) & 0xFFFF;
				for (size_t w = 0; w + 1 < words && !found; w += 2) {
					uint32_t op1 = read32(&file[p + 4 + AR_NAME + w * 4]);
					uint32_t op2 = read32(&file[p + 4 + AR_NAME + w * 4 + 4]);
					teaDecryptWith(actionReplaySeeds, &op1, &op2);
					found = op1 == codes[c][0] && op2 == codes[c][1];
				}
				p += 4 + AR_NAME + words * 4;
			}
			have += found;
		}
		if (have == n) {
			++*already;
			continue;
		}
		size_t const need = 4 + AR_NAME + n * 8;
		if (used + need > room || cheatsNow >= 0xFFFF) {
			++*left;
			continue;
		}
		write32(&file[used], (uint32_t) (n * 2));
		size_t length = 0;
		for (const char* s = cheats[i].description; *s && length < AR_NAME; ++s) {
			file[used + 4 + length++] = (uint8_t) (*s >= 0x20 && *s < 0x7F ? *s : '?');
		}
		memset(&file[used + 4 + length], ' ', AR_NAME - length);
		for (size_t c = 0; c < n; ++c) {
			uint32_t op1 = codes[c][0];
			uint32_t op2 = codes[c][1];
			teaEncryptWith(actionReplaySeeds, &op1, &op2);
			write32(&file[used + 4 + AR_NAME + c * 8], op1);
			write32(&file[used + 4 + AR_NAME + c * 8 + 4], op2);
		}
		used += need;
		++cheatsNow;
		++added;
	}
	if (!added) {
		free(file);
		return 0;
	}
	write32(file, (read32(file) & 0xFFFF0000u) | cheatsNow);
	unsigned const want = (unsigned) ((used + MAX_CLUSTER - 1) / MAX_CLUSTER);
	memset(&file[used], 0, want * MAX_CLUSTER - used);
	// The new cluster table, in the other sector: the old chain freed, a new one next-fit
	uint8_t* const newTable = tables[tableNow ^ 1];
	uint8_t* const newDirectory = directories[dirNow ^ 1];
	uint8_t* const tableCopy = malloc(MAX_TABLE);
	if (!tableCopy) {
		free(file);
		return -1;
	}
	memcpy(tableCopy, table, MAX_TABLE);
	unsigned freeCount = freeNow;
	cluster = first;
	for (unsigned i = 0; i < clusters; ++i) {
		unsigned const next = maxLink(tableCopy, cluster);
		write16Big(&tableCopy[2 * cluster], 0);
		++freeCount;
		cluster = next;
	}
	unsigned chain[MAX_END - MAX_FIRST];
	unsigned last = read16Big(&tableCopy[8]);
	unsigned found = 0;
	for (unsigned tries = 0, c = last + 1; found < want && tries < MAX_END - MAX_FIRST; ++tries, ++c) {
		if (c < MAX_FIRST || c >= MAX_END) {
			c = MAX_FIRST;
		}
		if (!maxLink(tableCopy, c)) {
			chain[found++] = c;
			last = c;
		}
	}
	if (found < want) {
		free(tableCopy);
		free(file);
		return -1;
	}
	for (unsigned i = 0; i < want; ++i) {
		write16Big(&tableCopy[2 * chain[i]], i + 1 < want ? chain[i + 1] : 0xFFFF);
		memcpy(&flash[MAX_TABLES + (size_t) chain[i] * MAX_CLUSTER], &file[i * MAX_CLUSTER], MAX_CLUSTER);
	}
	free(file);
	write16Big(&tableCopy[4], generation + 1);
	write16Big(&tableCopy[6], freeCount - want);
	write16Big(&tableCopy[8], last);
	maxSums(&tableCopy[MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tableCopy[0]);
	// The other sector keeps its own options area (sector 0's), as the program's copy does
	memcpy(newTable, tableCopy, 0x10);
	memcpy(&newTable[MAX_TABLE_SUMMED], &tableCopy[MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED);
	free(tableCopy);
	// The new directory, in the other copy
	memmove(newDirectory, directory, MAX_DIRECTORY);
	uint8_t* const newEntry = &newDirectory[slot * MAX_ENTRY];
	write16Big(&newEntry[0x1C], chain[0]);
	write16Big(&newEntry[0x1E], want);
	write16Big(&newDirectory[MAX_GENERATION], generation + 2);
	maxSums(newDirectory, MAX_SUMS, &newDirectory[MAX_SUMS]);
	return added;
}

// The MAX's copies in use: the directory and table whose checksums hold, the newer of each, and the
// highest generation of those whole. False when neither copy of one holds.
static bool maxCopies(uint8_t* flash, int* dirNow, int* tableNow, unsigned* generation) {
	uint8_t* const directories[2] = { &flash[MAX_DIRECTORIES], &flash[MAX_DIRECTORIES + MAX_DIRECTORY] };
	uint8_t* const tables[2] = { &flash[MAX_TABLES], &flash[MAX_TABLES + MAX_TABLE] };
	unsigned const dirGen[2] = { read16Big(&directories[0][MAX_GENERATION]), read16Big(&directories[1][MAX_GENERATION]) };
	unsigned const tableGen[2] = { read16Big(&tables[0][4]), read16Big(&tables[1][4]) };
	*dirNow = dirGen[0] > dirGen[1] ? 0 : 1;
	if (!maxSumsHold(directories[*dirNow], MAX_SUMS, &directories[*dirNow][MAX_SUMS])) {
		*dirNow ^= 1;
		if (!maxSumsHold(directories[*dirNow], MAX_SUMS, &directories[*dirNow][MAX_SUMS])) {
			return false;
		}
	}
	*tableNow = tableGen[0] > tableGen[1] ? 0 : 1;
	if (!maxSumsHold(&tables[*tableNow][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[*tableNow][0])) {
		*tableNow ^= 1;
		if (!maxSumsHold(&tables[*tableNow][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[*tableNow][0])) {
			return false;
		}
	}
	*generation = 0;
	for (int i = 0; i < 2; ++i) {
		if (maxSumsHold(directories[i], MAX_SUMS, &directories[i][MAX_SUMS]) && dirGen[i] > *generation) {
			*generation = dirGen[i];
		}
		if (maxSumsHold(&tables[i][MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tables[i][0]) && tableGen[i] > *generation) {
			*generation = tableGen[i];
		}
	}
	return *generation + 2 < 0xFFFF;
}

// A file's clusters, in order: how many, or 0 when its chain isn't whole.
static unsigned maxChain(const uint8_t* table, const uint8_t* entry, unsigned* chain) {
	unsigned const clusters = read16Big(&entry[0x1E]);
	if (!clusters || clusters > MAX_END - MAX_FIRST) {
		return 0;
	}
	uint8_t seen[(MAX_END + 7) / 8] = { 0 };
	unsigned cluster = read16Big(&entry[0x1C]);
	for (unsigned i = 0; i < clusters; ++i) {
		if (cluster < MAX_FIRST || cluster >= MAX_END || seen[cluster / 8] & 1 << (cluster % 8)) {
			return 0;
		}
		seen[cluster / 8] |= (uint8_t) (1 << (cluster % 8));
		chain[i] = cluster;
		cluster = maxLink(table, cluster);
	}
	return clusters;
}

int maxClear(uint8_t* flash, size_t size, bool keepMasters) {
	if (size < (size_t) MAX_TABLES + (size_t) MAX_END * MAX_CLUSTER) {
		return -1;
	}
	int dirNow, tableNow;
	unsigned generation;
	if (!maxCopies(flash, &dirNow, &tableNow, &generation)) {
		return -1;
	}
	uint8_t* const directories[2] = { &flash[MAX_DIRECTORIES], &flash[MAX_DIRECTORIES + MAX_DIRECTORY] };
	uint8_t* const tables[2] = { &flash[MAX_TABLES], &flash[MAX_TABLES + MAX_TABLE] };
	const uint8_t* const directory = directories[dirNow];
	const uint8_t* const table = tables[tableNow];
	static unsigned chain[MAX_END - MAX_FIRST];
	if (keepMasters) {
		// Each code list in its own clusters, its master codes (40000000) only, zeros after
		uint8_t* const file = malloc((size_t) (MAX_END - MAX_FIRST) * MAX_CLUSTER);
		uint8_t* const kept = malloc((size_t) (MAX_END - MAX_FIRST) * MAX_CLUSTER);
		if (!file || !kept) {
			free(file);
			free(kept);
			return -1;
		}
		for (size_t i = 0; i < MAX_ENTRIES; ++i) {
			const uint8_t* const entry = &directory[i * MAX_ENTRY];
			if (read16Big(&entry[0x1C]) == 0xFFFF || !(entry[0x1B] & MAX_LIST)) {
				continue;
			}
			unsigned const clusters = maxChain(table, entry, chain);
			if (!clusters) {
				continue;
			}
			size_t const fileSize = (size_t) clusters * MAX_CLUSTER;
			for (unsigned c = 0; c < clusters; ++c) {
				memcpy(&file[c * MAX_CLUSTER], &flash[MAX_TABLES + (size_t) chain[c] * MAX_CLUSTER], MAX_CLUSTER);
			}
			unsigned const cheatsThere = (unsigned) (read32(file) & 0xFFFF);
			size_t at = 4, used = 4;
			unsigned keptCount = 0;
			bool whole = true;
			for (unsigned c = 0; c < cheatsThere && whole; ++c) {
				uint32_t const word = at + 4 <= fileSize ? read32(&file[at]) : 0;
				size_t const length = 4 + AR_NAME + (word & 0xFFFF) * 4;
				whole = at + length <= fileSize;
				if (whole && (word & 0x40000000u)) {
					memcpy(&kept[used], &file[at], length);
					used += length;
					++keptCount;
				}
				at += length;
			}
			if (!whole) {
				continue;
			}
			write32(kept, (read32(file) & 0xFFFF0000u) | keptCount);
			memset(&kept[used], 0, fileSize - used);
			for (unsigned c = 0; c < clusters; ++c) {
				memcpy(&flash[MAX_TABLES + (size_t) chain[c] * MAX_CLUSTER], &kept[c * MAX_CLUSTER], MAX_CLUSTER);
			}
		}
		free(file);
		free(kept);
	}
	// The files gone (every one, or with keepMasters those that aren't code lists, such as the DS game
	// saves a used cartridge holds): their clusters freed in a new table, their entries erased in a new
	// directory, both in the other copies with generations above all
	uint8_t* const tableCopy = malloc(MAX_TABLE);
	if (!tableCopy) {
		return -1;
	}
	memcpy(tableCopy, table, MAX_TABLE);
	unsigned freeCount = read16Big(&table[6]);
	uint8_t* const newDirectory = directories[dirNow ^ 1];
	memmove(newDirectory, directory, MAX_DIRECTORY);
	for (size_t i = 0; i < MAX_ENTRIES; ++i) {
		uint8_t* const entry = &newDirectory[i * MAX_ENTRY];
		if (read16Big(&entry[0x1C]) == 0xFFFF || (keepMasters && (entry[0x1B] & MAX_LIST))) {
			continue;
		}
		unsigned const clusters = maxChain(table, entry, chain);
		for (unsigned c = 0; c < clusters; ++c) {
			if (maxLink(tableCopy, chain[c])) {
				write16Big(&tableCopy[2 * chain[c]], 0);
				++freeCount;
			}
		}
		memset(entry, 0xFF, MAX_ENTRY);
	}
	uint8_t* const newTable = tables[tableNow ^ 1];
	write16Big(&tableCopy[4], generation + 1);
	write16Big(&tableCopy[6], freeCount);
	maxSums(&tableCopy[MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED, &tableCopy[0]);
	memcpy(newTable, tableCopy, 0x10);
	memcpy(&newTable[MAX_TABLE_SUMMED], &tableCopy[MAX_TABLE_SUMMED], MAX_TABLE - MAX_TABLE_SUMMED);
	free(tableCopy);
	write16Big(&newDirectory[MAX_GENERATION], generation + 2);
	maxSums(newDirectory, MAX_SUMS, &newDirectory[MAX_SUMS]);
	return 0;
}
