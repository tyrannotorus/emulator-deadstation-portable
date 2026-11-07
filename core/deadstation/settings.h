// The player's settings, as Options shows them (ui/options/psp.cpp, and the game's own ones) and the files keep them, into what
// the emulation reads (g_Config): the game's own values, else the defaults (options.h, game()).
#pragma once
#include "dead_ui.h"

#include <string>

namespace deadstation::settings
{

// the game played, by its disc id (UMD_DATA.BIN), before the first apply(); none: every change is a default
void setGame(const std::string& discId);
// the game's values, else the defaults, into g_Config: at the start, and when the menu closes
void apply();
bool showFps();
// the game's size on the screen, for the screen's mode: 1 the PSP's, 2 twice it
int resolution();

// Options' values: the game's, else the default; a change kept for the game (changed() then says what to apply)
options::Store store();
enum Change : unsigned { Resolution = 1, Other = 2 };
unsigned changed();		// and cleared

}
