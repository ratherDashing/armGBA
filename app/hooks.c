/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "hooks.h"
#include "link.h"

#include <string.h>

// Low Memory needs the game's path, and the app gives the core the game itself.
static bool optionShown(const struct Option* option, bool shown) {
	return strcmp(option->key, "armgba_low_memory") && lcdOptionShown(option, shown);
}

static const char* const cardFiles[] = { ".raw", ".bin", NULL };

const struct AppHooks appHooks = {
	.pictureCame = lcdPictureCame,
	.pictureUpload = lcdPictureUpload,
	.pictureDraw = lcdPictureDraw,
	.rendererGoing = lcdRendererGoing,
	.coreScale = lcdCoreScale,
	.optionRead = lcdOptionRead,
	.optionShown = optionShown,
	.optionRows = lcdOptionRows,
	.groupRows = lcdGroupRows,
	.settingsDefaults = lcdSettingsDefaults,
	.settingRead = lcdSettingRead,
	.settingsWrite = lcdSettingsWrite,
	.netplayPlayer = libretroSetNetplayPlayer,
	.firmwareCrc = libretroFirmwareCrc,
	.sessionValue = netSessionValue,
	.takeSave = libretroTakeSave,
	.sessionEnded = libretroSessionEnded,
	.playOn = libretroPlayOn,
	.saveManagerFolders = libretroSaveManagerFolders,
	.cheevosConsole = netCheevosConsole,
	.discMenu = "e-Reader Cards",
	.disc = "Card",
	.discFiles = cardFiles,
};
