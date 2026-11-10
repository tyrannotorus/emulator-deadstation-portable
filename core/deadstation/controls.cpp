#include "controls.h"
#include "dead_ui.h"

#include "Core/HLE/sceCtrl.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/stat.h>

namespace deadstation::controls
{
namespace
{

// the Brick's buttons as SDL numbers them
enum : int { PadB = 0, PadA = 1, PadY = 2, PadX = 3, PadL1 = 4, PadR1 = 5, PadSelect = 6, PadStart = 7,
	PadFn1 = 9, PadFn2 = 10, PadMinus = 13, PadPlus = 14 };
enum : int { AxisL2 = 2, AxisR2 = 5 };

const std::vector<Control> Controls {
	{ Up, "Up", "" }, { Down, "Down", "" }, { Left, "Left", "" }, { Right, "Right", "" },
	{ Cross, "Cross", "" }, { Circle, "Circle", "" }, { Square, "Square", "" }, { Triangle, "Triangle", "" },
	{ L, "L", "" }, { R, "R", "" },
	{ Select, "Select", "" }, { Start, "Start", "" },
	{ SwapDpad, "Swap D-Pad", "Swaps the d-pad between the PSP's d-pad and its analog nub, for this game." },
	{ SaveState, "Save State", "Saves the game in the slot picked last." },
	{ LoadState, "Load State", "Loads the game from the slot picked last." },
	{ Screenshot, "Screenshot", "Saves a picture of the screen in Screenshots." },
};

// by position, as the PSP's buttons: the Brick's B (south) is Cross
const int Defaults[Count] = {
	HatUp, HatDown, HatLeft, HatRight, PadB, PadA, PadY, PadX, PadL1, PadR1,
	PadSelect, PadStart, Axis + AxisR2, PadFn2, PadFn1, None,
};

int bindings[Count];
SDL_Joystick *pad = nullptr;
std::string swapFile;
bool swapped = false;
// update() runs on the pad's thread, toMenu() and toGame() on the main one
std::mutex gameMutex;
bool menuHas = false;
// the functions (a bit each) held as the menu let go, kept from the game until released
uint32_t heldBack = 0;

// a function down for the game: held, and not since the menu
bool forGame(Function f)
{
	if (!held(f))
	{
		heldBack &= ~(1u << f);
		return false;
	}
	return (heldBack & (1u << f)) == 0;
}

// the defaults
std::string path()
{
	return recomp::home() + "/controls.cfg";
}

// the game's own: games/<disc id>.controls.cfg, "" without a game
std::string gamePath()
{
	return options::gameFile(".controls.cfg");
}

void writeTo(const std::string& file)
{
	if (file == gamePath())
		mkdir((recomp::home() + "/games").c_str(), 0755);
	std::ofstream out(file, std::ios::trunc);
	for (const Control& c : Controls)
		out << c.name << " = " << bindings[c.function] << "\n";
}

// a change: for the game (without a disc id, the defaults)
void save()
{
	writeTo(gamePath().empty() ? path() : gamePath());
}

}	// namespace

const std::vector<Control>& list()
{
	return Controls;
}

void saveAsDefault()
{
	writeTo(path());
	if (!gamePath().empty())
		remove(gamePath().c_str());
}

void load()
{
	for (int i = 0; i < Count; i++)
		bindings[i] = Defaults[i];
	// the game's, else the defaults
	const std::string own = gamePath();
	std::ifstream in(!own.empty() && std::ifstream(own).good() ? own : path());
	std::map<std::string, int> read;
	for (std::string line; std::getline(in, line); )
	{
		const size_t eq = line.find(" = ");
		if (eq != std::string::npos)
			read[line.substr(0, eq)] = std::atoi(line.c_str() + eq + 3);
	}
	for (const Control& c : Controls)
		if (auto r = read.find(c.name); r != read.end())
			bindings[c.function] = r->second;
}

int binding(Function f)
{
	return bindings[f];
}

std::string inputName(int input)
{
	switch (input)
	{
	case None: return "NONE";
	case PadB: return "B";
	case PadA: return "A";
	case PadY: return "Y";
	case PadX: return "X";
	case PadL1: return "L1";
	case PadR1: return "R1";
	case PadSelect: return "SELECT";
	case PadStart: return "START";
	case MenuButton: return "MENU";
	case PadFn1: return "FN1";
	case PadFn2: return "FN2";
	case PadMinus: return "MINUS";
	case PadPlus: return "PLUS";
	case HatUp: return "UP";
	case HatDown: return "DOWN";
	case HatLeft: return "LEFT";
	case HatRight: return "RIGHT";
	case Axis + AxisL2: return "L2";
	case Axis + AxisR2: return "R2";
	default:
		return input >= Axis ? "AXIS " + std::to_string(input - Axis) : "BUTTON " + std::to_string(input);
	}
}

void bind(Function f, int input)
{
	for (int& b : bindings)
		if (b == input)
			b = None;
	bindings[f] = input;
	save();
}

void unbind(Function f)
{
	bindings[f] = None;
	save();
}

void open(SDL_Joystick *joystick)
{
	pad = joystick;
}

bool pressed(int input)
{
	if (pad == nullptr || input == None)
		return false;
	if (input >= Axis)
		return SDL_JoystickGetAxis(pad, input - Axis) > 16384;
	if (input >= HatUp)
	{
		const Uint8 hat = SDL_JoystickNumHats(pad) > 0 ? SDL_JoystickGetHat(pad, 0) : 0;
		static const Uint8 Bits[] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
		return (hat & Bits[input - HatUp]) != 0;
	}
	return SDL_JoystickGetButton(pad, input) != 0;
}

bool held(Function f)
{
	return pressed(bindings[f]);
}

void setGame(const std::string& romPath)
{
	std::string name = romPath.substr(romPath.find_last_of('/') + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0)
		name.resize(dot);
	swapFile = recomp::home() + "/" + name + ".dpad";
	swapped = std::ifstream(swapFile).good();
}

bool dpadIsNub()
{
	return swapped;
}

void toggleDpad()
{
	swapped = !swapped;
	if (swapped)
		std::ofstream(swapFile) << "nub\n";
	else
		std::remove(swapFile.c_str());
}

void update()
{
	static const struct { Function function; u32 bit; } Buttons[] = {
		{ Cross, CTRL_CROSS }, { Circle, CTRL_CIRCLE }, { Square, CTRL_SQUARE }, { Triangle, CTRL_TRIANGLE },
		{ L, CTRL_LTRIGGER }, { R, CTRL_RTRIGGER }, { Select, CTRL_SELECT }, { Start, CTRL_START },
	};
	std::lock_guard<std::mutex> lock(gameMutex);
	if (menuHas)
		return;
	u32 bits = 0;
	for (const auto& b : Buttons)
		if (forGame(b.function))
			bits |= b.bit;
	const bool up = forGame(Up), down = forGame(Down), left = forGame(Left), right = forGame(Right);
	float x = 0.f, y = 0.f;
	if (swapped)
	{
		x = (right ? 1.f : 0.f) - (left ? 1.f : 0.f);
		y = (up ? 1.f : 0.f) - (down ? 1.f : 0.f);
	}
	else
		bits |= (up ? CTRL_UP : 0) | (down ? CTRL_DOWN : 0) | (left ? CTRL_LEFT : 0) | (right ? CTRL_RIGHT : 0);
	const u32 all = CTRL_CROSS | CTRL_CIRCLE | CTRL_SQUARE | CTRL_TRIANGLE | CTRL_LTRIGGER | CTRL_RTRIGGER | CTRL_SELECT | CTRL_START
			| CTRL_UP | CTRL_DOWN | CTRL_LEFT | CTRL_RIGHT;
	{
		static const bool logCtrl = getenv("DEADSTATION_PORTABLE_CTRLLOG") != nullptr;
		static u32 lastBits = 0;
		static float lastX = 0.f, lastY = 0.f;
		if (logCtrl && (bits != lastBits || x != lastX || y != lastY))
		{
			printf("ctrl %u: bits %x nub %.0f %.0f\n", SDL_GetTicks(), bits, x, y);
			fflush(stdout);
			lastBits = bits;
			lastX = x;
			lastY = y;
		}
	}
	__CtrlUpdateButtons(bits, all & ~bits);
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, x, y);
}

void toMenu()
{
	std::lock_guard<std::mutex> lock(gameMutex);
	menuHas = true;
}

void toGame()
{
	{
		std::lock_guard<std::mutex> lock(gameMutex);
		menuHas = false;
		heldBack = 0;
		for (int f = Up; f <= Start; f++)
			if (held((Function)f))
				heldBack |= 1u << f;
	}
	update();
}

}
