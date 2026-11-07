// The Brick's controls (trimui_inputd's Xbox 360 pad through SDL's joystick API) and what each does:
// the PSP's buttons and the emulator's own functions, remapped in Options > Controls and kept in
// <home>/controls.cfg. MENU is always the menu. The Brick has no stick, so the d-pad stands in for the PSP's nub when
// swapped (SwapDpad): remembered for each game.
#pragma once
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace deadstation::controls
{

// an input: a button (its number), a d-pad direction (HatUp..HatRight) or a trigger (Axis + its axis)
enum : int { None = -1, HatUp = 256, HatDown, HatLeft, HatRight, Axis = 512 };
constexpr int MenuButton = 8;

enum Function : int
{
	Up, Down, Left, Right, Cross, Circle, Square, Triangle, L, R, Select, Start,
	SwapDpad, SaveState, LoadState, Screenshot, Count
};
struct Control
{
	Function function;
	const char *name;
	const char *desc;
};
const std::vector<Control>& list();

// the game's own controls (options::gameFile, games/<disc id>.controls.cfg: each change kept there), else the defaults
// (<home>/controls.cfg, which saveAsDefault() writes from the game's, the game then following them)
void saveAsDefault();
void load();				// the game's, else the defaults, else the built-in ones
int binding(Function f);
std::string inputName(int input);
// one control does one thing: taken from whatever had it; saved at once
void bind(Function f, int input);
void unbind(Function f);

// the pad's state now
void open(SDL_Joystick *joystick);
bool pressed(int input);
bool held(Function f);

// the d-pad as the nub, for this game
void setGame(const std::string& romPath);
bool dpadIsNub();
void toggleDpad();

// the PSP's buttons now, to the game
void update();

}
