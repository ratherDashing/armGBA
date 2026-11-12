/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_APP_HOOKS_H
#define ARMGBA_APP_HOOKS_H

// What armGBA adds to the shared app (app-hooks.h has what each hook is for).

#include "app-hooks.h"

bool lcdPictureCame(void);
void lcdPictureUpload(void);
void lcdPictureDraw(SDL_Rect to);
void lcdRendererGoing(void);
float lcdCoreScale(void);
const char* lcdOptionRead(const struct Option* option);
bool lcdOptionShown(const struct Option* option, bool shown);
void lcdOptionRows(const struct Option* option);
void lcdGroupRows(const char* category);
void lcdSettingsDefaults(void);
bool lcdSettingRead(const char* key, const char* value, int n, bool yes);
void lcdSettingsWrite(FILE* f);
const char* netSessionValue(const char* key, const char* value);
uint32_t netCheevosConsole(const uint8_t* data, size_t size);

#endif
