// The in-game menu (MENU button), styled after NextUI's minarch: Continue, Save, Load, Options, Recomp Store, Quit,
// as in Deadcast and Deadstation 1. Drawn on the CPU by the shared menu library (ui/src/ui.h), shown through the panel's
// pictures (fb0 in NextUI's mode). The game is held while it is open; everything runs on the main thread, where the
// pad is read too.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Panel;

namespace deadstation::menu
{

// Switch: the game again in the other core (Options): the emulator ends and the launcher starts it again.
enum class Result { Open, Resume, Quit, Switch };

// the screen it draws on (again after a change of mode); the file NextUI started (its name is the menu's title), the disc played
void init(Panel *panel, const std::string& rom, const std::string& disc);
bool open();
// the menu, with a message over it: the game can't go on
bool openWithMessage(const std::string& text);
bool isOpen();
// raw pad input (controls.h's input numbers) while it is open
void input(int code, bool pressed);
// every main loop iteration while open: repeats held directions, redraws when needed
Result frame();

// the start page, "Loading..." over the game's file name, until its first frame
bool showStart();
bool starting();
void endStart();
void showStartError(const std::string& what);
// the first time a game runs, after the start page: a word about shader compilation's stutter, on its own page until pressed()
// says a button went down
void firstRunNotice(const std::function<bool()>& pressed);

// the save state slot picked last (FN2/FN1 use it), saved and loaded from outside the menu
bool saveSlot();
bool slotExists();
bool loadSlot();

// the emulator's Controls, from a hosted recomp module's Options: the pad opened only while that page is up, its presses
// read with events (input number, pressed)
void openHostedControls(std::function<bool()> openPad, std::function<void()> closePad,
		std::function<std::vector<std::pair<int, bool>>()> events);

}
