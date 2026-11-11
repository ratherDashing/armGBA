/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's options: what they are, given to the frontend; their values, as the frontend has them;
// and which of them show, for the game loaded and the Link Mode chosen. libretro.c's readOptions
// takes their values in.

#include "core.h"
#include "gba.h"
#include "lcd.h"
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct retro_core_options_v2* coreOptions;
// Which options were last shown, once set.
static bool optionsShown[80];
static bool optionsShownSet;

static void fillTurboPeriods(struct retro_core_option_v2_definition* definitions) {
	static char periods[TURBO_MOST - TURBO_FEWEST + 1][4];
	static char labels[TURBO_MOST - TURBO_FEWEST + 1][12];
	for (; definitions->key && strcmp(definitions->key, "armgba_turbo_period"); ++definitions) {
	}
	for (size_t i = 0; definitions->key && i < COUNT(periods); ++i) {
		snprintf(periods[i], sizeof(periods[i]), "%d", TURBO_FEWEST + (int) i);
		snprintf(labels[i], sizeof(labels[i]), "%d frames", TURBO_FEWEST + (int) i);
		definitions->values[i] = (struct retro_core_option_value) { periods[i], labels[i] };
	}
}

// Custom Screen Size's hundreds: 1 to 40.
static void fillScreenSizes(struct retro_core_option_v2_definition* definitions) {
	static char values[40][4];
	static char labels[40][8];
	for (; definitions->key; ++definitions) {
		if (strcmp(definitions->key, "armgba_lcd_width_hundreds") && strcmp(definitions->key, "armgba_lcd_height_hundreds")) {
			continue;
		}
		for (int v = 1; v <= 40; ++v) {
			snprintf(values[v - 1], sizeof(values[v - 1]), "%d", v);
			snprintf(labels[v - 1], sizeof(labels[v - 1]), "%d00", v);
			definitions->values[v - 1] = (struct retro_core_option_value) { values[v - 1], labels[v - 1] };
		}
		definitions->values[40] = (struct retro_core_option_value) { NULL, NULL };
	}
}

// Clock Offset's choices: -30 to 30 days, and -23 to 23 hours.
static void fillClockOffsets(struct retro_core_option_v2_definition* definitions) {
	static char values[61 + 47][4];
	static char labels[61 + 47][12];
	int n = 0;
	for (; definitions->key; ++definitions) {
		bool const days = !strcmp(definitions->key, "armgba_clock_days");
		if (!days && strcmp(definitions->key, "armgba_clock_hours")) {
			continue;
		}
		int const most = days ? 30 : 23;
		for (int i = 0; i <= 2 * most; ++i, ++n) {
			int const value = i - most;
			snprintf(values[n], sizeof(values[n]), "%d", value);
			snprintf(labels[n], sizeof(labels[n]), "%+d %s%s", value, days ? "day" : "hour", abs(value) == 1 ? "" : "s");
			definitions->values[i] = (struct retro_core_option_value) { values[n], value ? labels[n] : "0" };
		}
		definitions->values[2 * most + 1] = (struct retro_core_option_value) { NULL, NULL };
	}
}

// Older frontends take each option as "Label; value|value", the default first, made here from the
// definitions.
static void setVariables(retro_environment_t callback, const struct retro_core_option_v2_definition* definitions) {
	static struct retro_variable variables[96];
	static char text[96][1024];
	int n = 0;
	for (; definitions->key && n < 95; ++definitions, ++n) {
		size_t at = snprintf(text[n], sizeof(text[n]), "%s; %s", definitions->desc, definitions->default_value);
		for (int i = 0; definitions->values[i].value && at < sizeof(text[n]); ++i) {
			if (strcmp(definitions->values[i].value, definitions->default_value)) {
				at += snprintf(text[n] + at, sizeof(text[n]) - at, "|%s", definitions->values[i].value);
			}
		}
		variables[n] = (struct retro_variable) { definitions->key, text[n] };
	}
	variables[n] = (struct retro_variable) { NULL, NULL };
	callback(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

// Keys and values match saved configurations; labels are what the menu shows. showOptions hides
// the options that do nothing for the game and Link Mode.
static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armgba_skip_bios", "Skip BIOS Intro", NULL, "Starts games without the BIOS intro.", NULL, "system",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_bios", "BIOS", NULL,
	  "Nintendo's gba_bios.bin from the system folder, else armGBA's own. Applies on Restart.",
	  NULL, "system",
	  { { "nintendo", "Nintendo's (System Folder)" }, { "open-source", "armGBA's (Built In)" }, { NULL, NULL } }, "nintendo" },
	{ "armgba_cpu_clock", "CPU Clock", NULL,
	  "Eases slowdown; game speed and music stay the same.",
	  NULL, "system",
	  { { "25", "25%" }, { "50", "50%" }, { "75", "75%" }, { "100", "100%" }, { "125", "125%" }, { "150", "150%" },
	    { "175", "175%" }, { "200", "200%" }, { "225", "225%" }, { "250", "250%" }, { "275", "275%" }, { "300", "300%" },
	    { "325", "325%" }, { "350", "350%" }, { "375", "375%" }, { "400", "400%" }, { NULL, NULL } }, "100" },
	{ "armgba_cpu_clock_boot", "CPU Clock During Boot", NULL,
	  "100% runs the BIOS intro or boot ROM at normal speed, then CPU Clock applies once the game starts.",
	  NULL, "system",
	  { { "stock", "100%" }, { "clock", "CPU Clock" }, { NULL, NULL } }, "stock" },
	{ "armgba_low_memory", "Low Memory", NULL,
	  "Reads the game from its file as needed, using less memory. Best unzipped: a zipped game runs without its patch. Applies when a game is opened.",
	  NULL, "system",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_clock_source", "Clock Source", NULL,
	  "Real Time: the device's clock. Game Time: a clock of the game's own that runs only while the game runs.",
	  NULL, "clock",
	  { { "real", "Real Time" }, { "game", "Game Time" }, { NULL, NULL } }, "real" },
	{ "armgba_clock_speed", "Clock Speed", NULL,
	  "How fast Game Time runs compared to the console's own time.",
	  NULL, "clock",
	  { { "stopped", "Stopped" }, { "0.1x", "0.1x" }, { "0.25x", "0.25x" }, { "0.5x", "0.5x" }, { "0.75x", "0.75x" }, { "1x", "1x" },
	    { "1.25x", "1.25x" }, { "1.5x", "1.5x" }, { "2x", "2x" }, { "3x", "3x" }, { "4x", "4x" }, { "6x", "6x" }, { "10x", "10x" },
	    { "24x", "24x (a day an hour)" }, { "60x", "60x (an hour a minute)" }, { NULL, NULL } }, "1x" },
	{ "armgba_clock_days", "Clock Offset: Days", NULL,
	  "Moves the clock forwards or back. Moving it back can pause daily events until the clock catches up.",
	  NULL, "clock", { { NULL, NULL } }, "0" },
	{ "armgba_clock_hours", "Clock Offset: Hours", NULL,
	  "Moves the clock forwards or back by hours.",
	  NULL, "clock", { { NULL, NULL } }, "0" },
	{ "armgba_clock_reset_allowed", "Allow Clock Reset", NULL,
	  "Shows Reset Clock. Turns itself off after a reset.",
	  NULL, "clock",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_clock_reset", "Reset Clock", NULL,
	  "Game Boy: clears the clock as a dead battery would, and games that check ask for the time. GBA: back to the device's time, offsets at 0.",
	  NULL, "clock",
	  { { "no", "Off" }, { "reset", "Reset" }, { NULL, NULL } }, "no" },
	{ "armgba_gb_model", "Game Boy Model", NULL,
	  "The console Game Boy games run on. On the Advance, L or R stretches the picture. Applies on Restart.",
	  NULL, "system",
	  { { "Auto", NULL }, { "Game Boy", NULL }, { "Game Boy Pocket", "Game Boy Pocket / Light" }, { "Game Boy Color", NULL }, { "Game Boy Advance", NULL }, { NULL, NULL } }, "Auto" },
	{ "armgba_gb_boot_rom", "Game Boy Boot ROM", NULL,
	  "The start-up logo: Nintendo's boot ROM (else armGBA's), armGBA's, or none. Applies on Restart.",
	  NULL, "system",
	  { { "enabled", "Nintendo's (System Folder)" }, { "open-source", "armGBA's (Built In)" }, { "disabled", "Off" }, { NULL, NULL } }, "enabled" },
	{ "armgba_threaded_video", "Threaded Video", NULL,
	  "Draws on a second CPU core for speed. One Frame Late is faster but a frame behind.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { "one frame late", "One Frame Late" }, { NULL, NULL } }, "disabled" },
	{ "armgba_threaded_audio", "Threaded Audio", NULL,
	  "Mixes the sound on another CPU core for speed. The sound is the same.", NULL, "audio",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_color_correction", "GBA Color Correction", NULL, "Darker, softer colors, as the GBA's screen showed them. Costs a little speed.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_color_correction", "Game Boy Color Correction", NULL,
	  "Colors as the Game Boy Color's screen showed them.", NULL, "video",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_gb_screen", "Game Boy Screen", NULL,
	  "Original Game Boy colors: grays, the first model's, the Pocket's or the Light's backlight.", NULL, "video",
	  { { "grayscale", "Grayscale" }, { "measured", "Game Boy" }, { "pocket measured", "Game Boy Pocket" }, { "light", "Game Boy Light (Backlit)" }, { NULL, NULL } },
	  "grayscale" },
	{ "armgba_gb_contrast", "Game Boy Contrast", NULL,
	  "The original Game Boy's contrast wheel. The right stick turns it; R3 resets it to 50%.", NULL, "video",
	  { { "-5", "0%" }, { "-4", "10%" }, { "-3", "20%" }, { "-2", "30%" }, { "-1", "40%" }, { "0", "50%" },
	    { "1", "60%" }, { "2", "70%" }, { "3", "80%" }, { "4", "90%" }, { "5", "100%" }, { NULL, NULL } }, "0" },
	{ "armgba_gb_advance_border", "Game Boy on GBA: Border", NULL,
	  "On the Advance model: just the game, or the game with the black border around it.", NULL, "video",
	  { { "cropped", "Cropped" }, { "shown", "Shown" }, { NULL, NULL } }, "cropped" },
	{ "armgba_gb_advance_panel", "Game Boy on GBA: Screen", NULL,
	  "On the Advance model: show the game on the Advance's screen or the Color's, with that screen's colors and effects.",
	  NULL, "video", { { "advance", "Game Boy Advance" }, { "color", "Game Boy Color" }, { NULL, NULL } }, "advance" },
	{ "armgba_frame_skip", "Frame Skip", NULL,
	  "Skips drawing frames to keep up: Auto when the sound is about to run short, or this many after each frame shown. Not in netplay.",
	  NULL, "video", { { "disabled", "Off" }, { "auto", "Auto" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_effects", "CPU LCD Effects", NULL,
	  "Turns the effects below on or off together. Each one keeps its setting.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_ghosting", "LCD Ghosting", NULL,
	  "Changes fade in over a few frames, as on the consoles' slow LCDs. Flickering sprites need it.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "25", "25%" }, { "half", "50%" }, { "75", "75%" }, { "enabled", "100%" }, { NULL, NULL } },
	  "disabled" },
	{ "armgba_lcd_interleave", "Interlaced Rows", NULL,
	  "The Color's and Advance's screens update every other row each frame. With LCD Ghosting, updated rows fade faster.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_grid", "LCD Scale", NULL,
	  "Draws the picture larger, with room for Pixel Gaps, LCD Subpixels and the pixel shadow. Fit Screen draws it at the screen's own size.",
	  NULL, "video_lcd",
	  { { "disabled", "Off" }, { "fit", "Fit Screen" }, { "2", "2x" }, { "2.5", "2.5x" }, { "3", "3x" }, { "3.5", "3.5x" }, { "4", "4x" }, { "4.5", "4.5x" },
	    { "5", "5x" }, { "5.5", "5.5x" }, { "6", "6x" }, { "6.5", "6.5x" }, { "7", "7x" }, { "7.5", "7.5x" }, { "8", "8x" }, { NULL, NULL } },
	  "disabled" },
	{ "armgba_lcd_screen", "Screen Size", NULL,
	  "The size Fit Screen draws at, to show it pixel for pixel. Auto uses the app's window or the display's size where it can, else 1280x720.",
	  NULL, "video_lcd",
	  { { "auto", "Auto" }, { "480x320", "480x320" }, { "640x480", "640x480" }, { "720x480", "720x480" }, { "720x720", "720x720" },
	    { "800x480", "800x480" }, { "854x480", "854x480" }, { "960x544", "960x544" }, { "960x640", "960x640" }, { "1024x600", "1024x600" },
	    { "1024x768", "1024x768" }, { "1240x1080", "1240x1080" }, { "1280x720", "1280x720" }, { "1280x800", "1280x800" },
	    { "1280x960", "1280x960" }, { "1334x750", "1334x750" }, { "1600x900", "1600x900" }, { "1920x1080", "1920x1080" },
	    { "1920x1152", "1920x1152" }, { "1920x1200", "1920x1200" }, { "2400x1080", "2400x1080" }, { "2560x1440", "2560x1440" },
	    { "3840x2160", "3840x2160" }, { "custom", "Custom" }, { NULL, NULL } },
	  "auto" },
	{ "armgba_lcd_width_hundreds", "Custom Width (Hundreds)", NULL,
	  "Custom's width: these hundreds plus the tens and ones below.", NULL, "video_lcd", { { NULL, NULL } }, "12" },
	{ "armgba_lcd_width_tens", "Custom Width (Tens)", NULL,
	  "Custom's width: the hundreds above, these tens and the ones below.", NULL, "video_lcd",
	  { { "0", "+0" }, { "10", "+10" }, { "20", "+20" }, { "30", "+30" }, { "40", "+40" }, { "50", "+50" }, { "60", "+60" }, { "70", "+70" }, { "80", "+80" }, { "90", "+90" }, { NULL, NULL } }, "80" },
	{ "armgba_lcd_width_ones", "Custom Width (Ones)", NULL,
	  "Custom's width: the hundreds and tens above plus these. Screens are an even number of pixels across.", NULL, "video_lcd",
	  { { "0", "+0" }, { "2", "+2" }, { "4", "+4" }, { "6", "+6" }, { "8", "+8" }, { NULL, NULL } }, "0" },
	{ "armgba_lcd_height_hundreds", "Custom Height (Hundreds)", NULL,
	  "Custom's height: these hundreds plus the tens and ones below.", NULL, "video_lcd", { { NULL, NULL } }, "7" },
	{ "armgba_lcd_height_tens", "Custom Height (Tens)", NULL,
	  "Custom's height: the hundreds above, these tens and the ones below.", NULL, "video_lcd",
	  { { "0", "+0" }, { "10", "+10" }, { "20", "+20" }, { "30", "+30" }, { "40", "+40" }, { "50", "+50" }, { "60", "+60" }, { "70", "+70" }, { "80", "+80" }, { "90", "+90" }, { NULL, NULL } }, "20" },
	{ "armgba_lcd_height_ones", "Custom Height (Ones)", NULL,
	  "Custom's height: the hundreds and tens above plus these. Screens are an even number of pixels tall.", NULL, "video_lcd",
	  { { "0", "+0" }, { "2", "+2" }, { "4", "+4" }, { "6", "+6" }, { "8", "+8" }, { NULL, NULL } }, "0" },
	{ "armgba_lcd_gaps", "Pixel Gaps", NULL,
	  "The gaps between the screen's pixels. Leave off with armGBA's LCD shaders.",
	  NULL, "video_lcd", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_lcd_subpixels", "LCD Subpixels", NULL,
	  "The red, green and blue stripes in each pixel of the Color's and Advance's screens. 100% is the stripes alone.",
	  NULL, "video_lcd",
	  { { "0", "Off" }, { "10", "10%" }, { "20", "20%" }, { "30", "30%" }, { "40", "40%" }, { "50", "50%" }, { "60", "60%" }, { "70", "70%" },
	    { "80", "80%" }, { "90", "90%" }, { "100", "100%" }, { NULL, NULL } },
	  "0" },
	{ "armgba_lcd_shadow", "Game Boy Pixel Shadow", NULL,
	  "Which way the Game Boy's and Pocket's pixels cast a shadow, or Off.",
	  NULL, "video_lcd",
	  { { "off", "Off" }, { "up", "Up" }, { "up right", "Up Right" }, { "right", "Right" }, { "down right", "Down Right" }, { "down", "Down" },
	    { "down left", "Down Left" }, { "left", "Left" }, { "up left", "Up Left" }, { NULL, NULL } },
	  "off" },
	{ "armgba_lcd_shadow_distance", "Game Boy Pixel Shadow Distance", NULL,
	  "How far the pixel shadow falls, in pixels. In the app, also how far Tilt moves it.",
	  NULL, "video_lcd",
	  { { "0.5", "0.5" }, { "1.0", "1" }, { "1.5", "1.5" }, { "2.0", "2" }, { "2.5", "2.5" }, { NULL, NULL } },
	  "1.5" },
	{ "armgba_lcd_threads", "LCD Effect Threads", NULL,
	  "CPU cores for the LCD effects. Auto leaves one core free.",
	  NULL, "video_lcd", { { "auto", "Auto" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { NULL, NULL } }, "auto" },
	{ "armgba_low_pass_filter", "Low-Pass Filter", NULL,
	  "Softens high notes, as the consoles' small speakers did. Higher is softer.", NULL, "audio",
	  { { "disabled", "Off" }, { "20", "20%" }, { "40", "40%" }, { "60", "60%" }, { "80", "80%" }, { NULL, NULL } }, "disabled" },
	{ "armgba_left_stick_dpad", "Left Stick as D-Pad", NULL,
	  "The left stick presses the D-pad too, unless it tilts a motion sensor cartridge.", NULL, "input",
	  { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_turbo_period", "Turbo Button Period", NULL,
	  "Frames per press while a turbo button is held: X/Y for A/B, L3/R3 for L/R on GBA.", NULL, "input",
	  { { NULL, NULL } }, "4" },
	{ "armgba_gb_tilt", "Tilt Control", NULL,
	  "What tilts motion sensor cartridges.",
	  NULL, "input",
	  { { "left stick", "Left Stick" }, { "right stick", "Right Stick" }, { "d-pad", "D-Pad" }, { NULL, NULL } }, "left stick" },
	{ "armgba_solar_sensor", "Solar Sensor", NULL,
	  "The sunlight a solar sensor cartridge sees. The right stick changes it while playing.", NULL, "input",
	  { { "0", "0 (Dark)" }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", "10 (Full Sun)" }, { NULL, NULL } }, "5" },
	{ "armgba_restart_buttons", "Restart Combo", NULL,
	  "Held a second, restarts that player's console. Select + Start may quit RetroArch.",
	  NULL, "input",
	  { { "l r start", "L + R + Start" }, { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "select start a b", "Select + Start + A + B" }, { "off", "Off" }, { NULL, NULL } },
	  "l r start" },
	{ "armgba_link_mode", "Link Mode", NULL,
	  "Off: one console. Together: every console on every device. Separate: one console per device. GameCube: link to Dolphin.",
	  NULL, "link",
	  { { "off", "Off" }, { "together", "Together" }, { "separate", "Separate" }, { "gamecube", "GameCube" }, { NULL, NULL } },
	  "off" },
	{ "armgba_shared_controls", "Shared Controls", NULL,
	  "With Link Mode Off: Everyone plays at once, or Pass Around hands control to one player at a time.",
	  NULL, "link", { { "off", "Off" }, { "everyone", "Everyone" }, { "pass around", "Pass Around" }, { NULL, NULL } }, "off" },
	{ "armgba_link_players", "Number of Players", NULL,
	  "How many consoles run. Four GBAs need a fast device. Applies on Restart.",
	  NULL, "link_players", { { "2", NULL }, { "3", NULL }, { "4", NULL }, { NULL, NULL } }, "2" },
	{ "armgba_player2_game", "Player 2's Game", NULL,
	  "Player 2's game, from player 1's folder. Players 3 and 4 play player 1's. Applies on Restart.",
	  NULL, "link_players", { { "same", "Same Game" }, { NULL, NULL } }, "same" },
	{ "armgba_link_cartridge", "Cartridge", NULL,
	  "Removed: no cartridge, for a single-pak game sent over the cable. Applies on Restart.",
	  NULL, "link_connection", { { "inserted", "Inserted" }, { "removed", "Removed" }, { NULL, NULL } }, "inserted" },
	{ "armgba_link_port", "GBA Link Port", NULL,
	  "The Link Cable, or the Wireless Adapter for games that use it. Applies on Restart.",
	  NULL, "link_connection", { { "cable", "Link Cable" }, { "wireless", "Wireless Adapter" }, { NULL, NULL } }, "cable" },
	{ "armgba_gb_link_port", "Game Boy Link Port", NULL,
	  "The Link Cable for two players, or the Four Player Adapter, which 3 or 4 always use. Applies on Restart.",
	  NULL, "link_connection", { { "cable", "Link Cable" }, { "adapter", "Four Player Adapter" }, { NULL, NULL } }, "cable" },
	{ "armgba_gb_link_timing", "Game Boy Link Timing", NULL,
	  "Standard: smooth over Wi-Fi, fine for trades. Exact: in step like a real cable, slower over Wi-Fi.",
	  NULL, "link_connection", { { "standard", "Standard" }, { "exact", "Exact" }, { NULL, NULL } }, "standard" },
	{ "armgba_gb_printer", "Game Boy Printer", NULL,
	  "With Link Mode Off, a Game Boy Printer on the link port. Printouts are saved as pictures in the save folder.",
	  NULL, "link_connection", { { "off", "Off" }, { "on", "On" }, { NULL, NULL } }, "off" },
	{ "armgba_gamecube_address", "Dolphin Address", NULL,
	  "Where Dolphin runs: found on this device or the local network, or this device only.",
	  NULL, "link_connection", { { "auto", "Find on the Network" }, { "127.0.0.1", "This Device" }, { NULL, NULL } }, "auto" },
	{ "armgba_link_screens", "Screen Layout", NULL,
	  "How the screens are arranged. R2 changes it while playing.", NULL, "link_screens",
	  { { "side by side", "Side by Side" }, { "top and bottom", "Top and Bottom" }, { "player 1", "Player 1 Only" }, { "player 2", "Player 2 Only" }, { "player 3", "Player 3 Only" }, { "player 4", "Player 4 Only" }, { NULL, NULL } },
	  "side by side" },
	{ "armgba_link_l2", "L2 Button", NULL,
	  "Outside netplay, L2 fast-forwards or goes back a layout. In netplay it always goes back.",
	  NULL, "link_screens", { { "fast forward", "Fast-Forward" }, { "previous screens", "Previous Layout" }, { NULL, NULL } }, "fast forward" },
	{ "armgba_r2_follows", "Screen Layout Control", NULL,
	  "In netplay, whose L2 and R2 change this device's screens. Auto: this device's player.",
	  NULL, "link_screens",
	  { { "auto", "Auto" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "auto" },
	{ "armgba_switch_controller", "Switch Controller Combo", NULL,
	  "Moves controller 1 to the next console (not in netplay). With Pass Around, it takes over.",
	  NULL, "link_screens",
	  { { "start l2 r2", "Start + L2 + R2" }, { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "start l3", "Start + L3" }, { "start r3", "Start + R3" }, { "off", "Off" }, { NULL, NULL } },
	  "start l2 r2" },
	{ "armgba_player_numbers", "Show Player Numbers", NULL,
	  "Each player's number, faintly, at the top left of their screen.",
	  NULL, "link_screens", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "enabled" },
	{ "armgba_send_save", "Send Save", NULL,
	  "Save Manager: sends the chosen save to the linked device. Newest first.",
	  NULL, "saves", { { "off", "Off" }, { NULL, NULL } }, "off" },
	{ "armgba_receive_into", "Receive Saves Into", NULL,
	  "Save Manager: where a received save goes. Auto: player 2 on the host, yours elsewhere.",
	  NULL, "saves",
	  { { "auto", "Auto" }, { "1", "Your Save" }, { "2", "Player 2's Save" }, { "3", "Player 3's Save" }, { "4", "Player 4's Save" }, { NULL, NULL } },
	  "auto" },
	{ "armgba_back_up_save", "Back Up Save", NULL,
	  "Copies a player's save to a file named for the time. The game carries on.",
	  NULL, "saves", { { "off", "Off" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "off" },
	{ "armgba_load_save", "Load Save", NULL,
	  "Loads a backup or another player's save into a console, backing up the old one.",
	  NULL, "saves", { { "off", "Off" }, { NULL, NULL } }, "off" },
	{ "armgba_load_into", "Load Save Into", NULL,
	  "The player Load Save gives the save to.",
	  NULL, "saves", { { "same", "Same Player" }, { "1", "Player 1" }, { "2", "Player 2" }, { "3", "Player 3" }, { "4", "Player 4" }, { NULL, NULL } }, "same" },
	{ "armgba_keep_progress", "Keep My Netplay Progress", NULL,
	  "In Together, your progress on your own save is kept in it when the session ends.",
	  NULL, "saves_settings", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "disabled" },
	{ "armgba_new_save", "New Players' Saves", NULL,
	  "A new player's first save: new, or a copy of player 1's. Applies on Restart.",
	  NULL, "saves_settings", { { "new", "Create New Save" }, { "copy", "Copy Player 1's Save" }, { NULL, NULL } }, "new" },
	{ "armgba_player1_saves", "Player 1's Save Folder", NULL,
	  "Player 1's save: in the save folder, or in armGBA/Player 1. Applies when a game starts.",
	  NULL, "saves_settings", { { "retroarch", "Save Folder" }, { "armgba", "armGBA/Player 1 Folder" }, { NULL, NULL } }, "retroarch" },
	{ "armgba_keep_saves", "Backups Kept", NULL,
	  "How many backups to keep per player. Older ones are deleted.",
	  NULL, "saves_settings", { { "5", "5" }, { "10", "10" }, { "20", "20" }, { "all", "All" }, { NULL, NULL } }, "10" },
	{ "armgba_list_saves", "Backups Listed", NULL,
	  "How many of each player's newest backups Load Save lists.",
	  NULL, "saves_settings", { { "10", "10" }, { "20", "20" }, { "40", "40" }, { "80", "80" }, { "126", "126" }, { NULL, NULL } }, "10" },
	{ "armgba_safe_saves", "Safe Saves", NULL,
	  "Writes saves to a new file first, so a crash can't leave a broken save.",
	  NULL, "saves_settings", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "enabled" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category optionCategories[] = {
	{ "system", "System", "The console the game runs on and how it starts." },
	{ "clock", "Clock", "The cartridge's clock, for games that keep the time. In netplay, every player needs the same Clock options." },
	{ "video", "Video", "Colors, the picture and how it is drawn." },
	// Video's own group, and Link's and Saves': the standalone frontend shows them on their category's
	// page, under their names.
	{ "video_lcd", "Video: LCD Effects",
	  "The core draws these on the CPU. armGBA's LCD shader presets can draw all of them on the GPU instead." },
	{ "audio", "Audio", "How the sound is filtered." },
	{ "input", "Input", "The left stick, turbo, restart buttons and cartridge sensors." },
	{ "link", "Link", "Playing together: Link Mode, and the link's players, connection, screens and controls." },
	{ "link_players", "Link: Players", "How many consoles, and player 2's game." },
	{ "link_connection", "Link: Connection", "The link cable, the Wireless Adapter, the Four Player Adapter and the GameCube." },
	{ "link_screens", "Link: Screens and Controllers", "How the consoles' screens are shown, and moving a controller between them." },
	{ "saves", "Saves", "Backing up, loading and sending saves." },
	{ "saves_settings", "Saves: Settings", "Where saves are kept, new players' saves, backups, Safe Saves and netplay progress." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 optionsV2 = { optionCategories, optionDefinitions };

const char* option(const char* key) {
	struct retro_variable variable = { key, NULL };
	return environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : NULL;
}

bool optionIs(const char* key, const char* wanted) {
	const char* const value = option(key);
	return value && !strcmp(value, wanted);
}


// The number for the option's value, or fallback if the value isn't one of the choices.
int optionChoice(const char* key, const struct Choice* choices, size_t count, int fallback) {
	const char* const value = option(key);
	for (size_t i = 0; value && i < count; ++i) {
		if (!strcmp(value, choices[i].value)) {
			return choices[i].number;
		}
	}
	return fallback;
}

bool togetherWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "together");
}

bool separateWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "separate");
}

bool gamecubeWanted(void) {
	const char* value = option("armgba_link_mode");
	return value && !strcmp(value, "gamecube");
}

int playersWanted(void) {
	const char* value = option("armgba_link_players");
	return clampInt(value ? atoi(value) : 2, 2, MAX_PLAYERS);
}

// Three or four Game Boys always use the Four Player Adapter.
bool adapterWanted(int count) {
	const char* value = option("armgba_gb_link_port");
	return count > 2 || (value && !strcmp(value, "adapter"));
}

// Hides the options that do nothing for the game and Link Mode. RetroArch calls it when an option
// changes, before the core has read it, so it reads the options itself; returns whether that changed
// which are shown.
bool showOptions(void) {
	bool const advance = gba && !gameBoy;
	int const devices = advance ? gba->gpio.devices : 0;
	bool const together = togetherWanted();
	bool const linked = together || separateWanted();
	bool const effects = optionIs("armgba_lcd_effects", "enabled");
	bool const scaled = effects && !optionIs("armgba_lcd_grid", "disabled");
	enum LibretroGbClock const gbClock = gameBoy ? libretroGbClock() : LIBRETRO_GB_NO_CLOCK;
	bool const clock = devices & HW_RTC || gbClock;
	bool const gameTime = optionIs("armgba_clock_source", "game");
	const struct {
		const char* key;
		bool shown;
	} options[] = {
		{ "armgba_skip_bios", !gameBoy },
		{ "armgba_bios", !gameBoy },
		{ "armgba_cpu_clock", true },
		{ "armgba_cpu_clock_boot", !optionIs("armgba_cpu_clock", "100") },
		{ "armgba_low_memory", !gameBoy },
		{ "armgba_clock_source", clock },
		{ "armgba_clock_speed", clock && gameTime },
		{ "armgba_clock_days", clock && gbClock != LIBRETRO_GB_HUC3_CLOCK },
		{ "armgba_clock_hours", clock && gbClock != LIBRETRO_GB_HUC3_CLOCK },
		{ "armgba_clock_reset_allowed", gbClock == LIBRETRO_GB_MBC3_CLOCK || devices & HW_RTC },
		{ "armgba_clock_reset", (gbClock == LIBRETRO_GB_MBC3_CLOCK || devices & HW_RTC) && optionIs("armgba_clock_reset_allowed", "enabled") },
		{ "armgba_threaded_video", !gameBoy },
		{ "armgba_threaded_audio", !gameBoy },
		{ "armgba_lcd_ghosting", effects },
		{ "armgba_lcd_grid", effects },
		{ "armgba_lcd_screen", effects && optionIs("armgba_lcd_grid", "fit") },
		{ "armgba_lcd_width_hundreds", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_width_tens", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_width_ones", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_height_hundreds", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_height_tens", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_height_ones", effects && optionIs("armgba_lcd_grid", "fit") && optionIs("armgba_lcd_screen", "custom") },
		{ "armgba_lcd_gaps", scaled },
		{ "armgba_lcd_subpixels", scaled && lcdHasSubpixels() },
		{ "armgba_lcd_shadow", scaled && libretroLcdShadow() },
		{ "armgba_lcd_shadow_distance", scaled && libretroLcdShadow() && !optionIs("armgba_lcd_shadow", "off") },
		{ "armgba_color_correction", !gameBoy },
		{ "armgba_solar_sensor", !gameBoy && (!advance || devices & HW_LIGHT_SENSOR) },
		{ "armgba_gb_tilt", (!gameBoy && !advance) || devices & (HW_TILT | HW_GYRO) || (gameBoy && tiltCartridge) },
		{ "armgba_link_port", !gameBoy && linked },
		{ "armgba_gb_model", !advance },
		{ "armgba_gb_boot_rom", !advance },
		{ "armgba_gb_color_correction", !advance },
		{ "armgba_gb_screen", !advance },
		{ "armgba_gb_contrast", lcdCastsShadow() },
		{ "armgba_gb_advance_border", !advance && (gbOnAdvance || optionIs("armgba_gb_model", "Game Boy Advance")) },
		{ "armgba_gb_advance_panel", !advance && (gbOnAdvance || optionIs("armgba_gb_model", "Game Boy Advance")) },
		{ "armgba_lcd_interleave", effects && lcdHasSubpixels() },
		{ "armgba_lcd_threads", effects && (!optionIs("armgba_lcd_ghosting", "disabled") || !optionIs("armgba_lcd_grid", "disabled") || optionIs("armgba_lcd_interleave", "enabled")) },
		{ "armgba_gb_link_port", !advance && together },
		{ "armgba_gb_link_timing", !advance && separateWanted() },
		{ "armgba_gb_printer", !advance && optionIs("armgba_link_mode", "off") },
		{ "armgba_link_players", together },
		{ "armgba_link_screens", together },
		{ "armgba_link_l2", together },
		{ "armgba_r2_follows", together },
		{ "armgba_player_numbers", together },
		{ "armgba_switch_controller", together || optionIs("armgba_shared_controls", "pass around") },
		{ "armgba_shared_controls", optionIs("armgba_link_mode", "off") },
		{ "armgba_player2_game", together },
		{ "armgba_new_save", together },
		{ "armgba_load_into", together && !noGameLoaded() && !netplaySeen && !linkSession() },
		{ "armgba_back_up_save", !noGameLoaded() && !netplayJoined },
		{ "armgba_load_save", !noGameLoaded() && !netplaySeen && !linkSession() },
		{ "armgba_send_save", noGameLoaded() },
		{ "armgba_receive_into", noGameLoaded() },
		{ "armgba_player1_saves", !noGameLoaded() },
		{ "armgba_keep_progress", !noGameLoaded() },
		{ "armgba_list_saves", !noGameLoaded() },
		{ "armgba_link_cartridge", advance && (separateWanted() || gamecubeWanted()) },
		{ "armgba_gamecube_address", advance && gamecubeWanted() },
	};
	enum { OPTIONS = COUNT(options) };
	_Static_assert(OPTIONS <= COUNT(optionsShown), "optionsShown has room for every option shown and hidden here");
	bool changed = false;
	for (int i = 0; i < OPTIONS; ++i) {
		if (!optionsShownSet || optionsShown[i] != options[i].shown) {
			optionsShown[i] = options[i].shown;
			struct retro_core_option_display display = { options[i].key, options[i].shown };
			environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
			changed = true;
		}
	}
	optionsShownSet = true;
	return changed;
}

// Sets the options again after a list of choices changed. That shows every option, so they are
// hidden again.
void setOptions(void) {
	environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, coreOptions);
	optionsShownSet = false;
	showOptions();
}

// Gives the frontend the options: as definitions it can show and hide, returning true, or to an older
// frontend as plain variables.
bool optionsGive(retro_environment_t callback) {
	fillTurboPeriods(optionDefinitions);
	fillClockOffsets(optionDefinitions);
	fillScreenSizes(optionDefinitions);
	unsigned version = 0;
	if (callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &optionsV2);
		coreOptions = &optionsV2;
		struct retro_core_options_update_display_callback display = { showOptions };
		callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &display);
		return true;
	}
	setVariables(callback, optionDefinitions);
	return false;
}

struct retro_core_option_v2_definition* optionDefinition(const char* key) {
	for (struct retro_core_option_v2_definition* definition = optionDefinitions; definition->key; ++definition) {
		if (!strcmp(definition->key, key)) {
			return definition;
		}
	}
	return NULL;
}
