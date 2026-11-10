#include "menu.h"
#include "controls.h"
#include "dead_ui.h"
#include "log.h"
#include "settings.h"
#include "states.h"
#include "panel.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

// The emulator's side of the menu (ui/src/ui.h draws it): its settings for Options (settings.h), its own
// page (Controls), its save states, the start page, and the screen and pad.
namespace deadstation::menu
{
namespace
{

using ui::Item;
using ui::Row;
using ui::Screen;
using ui::entry;
using ui::S;

// the pages on the screen: the panel's pictures
class PanelDisplay : public ui::Display
{
public:
	Panel *panel = nullptr;
	bool begin(int& width, int& height) override
	{
		return panel != nullptr && panel->beginPictures(width, height);
	}
	void show(const u32 *pixels) override
	{
		panel->showPicture(pixels);
	}
	void end(After after) override
	{
		panel->endPictures(after == After::LastFrame ? Panel::After::LastFrame
				: after == After::NextFrame ? Panel::After::NextFrame : Panel::After::Stay);
	}
};
PanelDisplay display;

struct State
{
	std::string rom;				// the file NextUI started
	std::string disc;				// the disc played
	std::vector<u8> frame;			// the game's, when the menu opened (a saved state's picture)
	int frameWidth = 0;
	int frameHeight = 0;
	int slot = 0;
	int previewSlot = -1;			// the slot shown in the preview
	bool stateExists = false;
	std::vector<u8> preview;		// its picture
	int previewWidth = 0;
	int previewHeight = 0;
	int awaiting = -1;				// a Controls row waiting for a control (its function)
};
State st;

std::string fileName(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string displayName()
{
	const std::string name = ui::gameName(st.rom);
	return name.empty() ? fileName(st.rom) : name;
}

// the Controls page's rows: what each is bound to now
void refreshBindings(Screen& screen)
{
	const auto& list = controls::list();
	for (size_t i = 0; i < screen.items.size() && i < list.size(); i++)
	{
		screen.items[i].values = { controls::inputName(controls::binding(list[i].function)) };
		screen.items[i].value = 0;
	}
}

Screen bindingsScreen()
{
	Screen screen;
	screen.title = "Controls";
	for (const controls::Control& control : controls::list())
	{
		Item item;
		item.name = control.name;
		item.desc = control.desc;
		item.row = Row::Binding;
		const controls::Function f = control.function;
		item.confirm = [f] {
			st.awaiting = f;
			ui::awaitControl(true);
		};
		item.clear = [f] {
			controls::unbind(f);
			refreshBindings(ui::page());
		};
		screen.items.push_back(std::move(item));
	}
	if (options::hasGame())
		screen.items.push_back(ui::saveAsDefaultEntry("Use these controls for all games.", controls::saveAsDefault));
	refreshBindings(screen);
	return screen;
}

void updatePreview()
{
	if (st.previewSlot == st.slot)
		return;
	st.previewSlot = st.slot;
	st.preview.clear();
	st.stateExists = states::exists(st.slot);
	if (st.stateExists && !states::picture(st.slot, st.preview, st.previewWidth, st.previewHeight))
		st.preview.clear();
}

// the selected slot's saved state, at the right of a page drawn at ox, the slots under it
void drawPreview(int ox)
{
	Canvas& c = ui::canvas();
	const int hw = c.width() / 2;
	const int hh = c.height() / 2;
	const int pw = hw + S(4 * 2);
	const int ph = hh + S(4 * 2 + 6 + 4);
	int x = ox + c.width() - pw - ui::Padding;
	int y = (c.height() - ph) / 2;
	c.roundRect(x, y, pw, ph, S(4), ui::White);
	x += S(4);
	y += S(4);
	c.fill(x, y, hw, hh, ui::Black);
	updatePreview();
	if (!st.preview.empty())
		c.picture(st.preview.data(), st.previewWidth, st.previewHeight, x, y, hw, hh);
	else
		ui::drawMessage(ui::fonts().large, st.stateExists ? "No Preview" : "Empty Slot", x, y, hw, hh);
	// the slots: the one picked a black dot, the others small gray ones
	const int px = x + (hw - S(15 * states::SlotCount)) / 2;
	const int py = y + hh + S(4);
	for (int i = 0; i < states::SlotCount; i++)
	{
		const int cx = px + S(15 * i) + S(15) / 2;
		const int cy = py + S(3);
		if (i == st.slot)
			c.circle(cx, cy, S(3), ui::Black);
		else
			c.circle(cx, cy, S(1), ui::LightGray);
	}
}


bool saveState()
{
	if (!states::save(states::slotPath(st.slot), st.frame, st.frameWidth, st.frameHeight))
	{
		ui::message("The game could not be saved.");
		return false;
	}
	st.previewSlot = -1;
	return true;
}

bool loadState()
{
	if (!states::load(states::slotPath(st.slot)))
	{
		ui::message("The game could not be loaded.");
		return false;
	}
	return true;
}

// the main page's Save and Load, on the slot picked with Left/Right, its saved state beside them
ui::Host host()
{
	ui::Host host;
	host.disc = st.disc;
	host.save = entry("Save", "", [] {
		ui::work("Saving...", fileName(st.rom), [] {
			if (saveState())
				ui::finish(ui::Result::Resume);
		});
	});
	host.load = entry("Load", "", [] {
		updatePreview();
		if (st.stateExists)
			ui::work("Loading...", fileName(st.rom), [] {
				if (loadState())
					ui::finish(ui::Result::Resume);
			});
	});
	for (Item *item : { &host.save, &host.load })
	{
		item->shift = [](int step) { st.slot = (st.slot + step + states::SlotCount) % states::SlotCount; };
		item->aside = drawPreview;
	}
	host.version = "Deadstation Portable " DEADSTATION_VERSION;
	host.emulatorSettings = settings::store();
	if (options::hasGame())
		host.saveAsDefault = options::saveAsDefault;
	host.controls = [] { ui::push(bindingsScreen()); };
	host.localMultiplayer = [] { ui::message("Local Multiplayer is not available yet."); };
	return host;
}

ui::Button menuButton(int code)
{
	switch (code)
	{
	case controls::HatUp: return ui::Button::Up;
	case controls::HatDown: return ui::Button::Down;
	case controls::HatLeft: return ui::Button::Left;
	case controls::HatRight: return ui::Button::Right;
	case 1: return ui::Button::A;			// the Brick's A (east)
	case 0: return ui::Button::B;			// its B (south)
	case 3: return ui::Button::X;			// its X (north)
	case controls::MenuButton: return ui::Button::Menu;
	default: return ui::Button::None;
	}
}

}	// namespace

void init(Panel *panel, const std::string& rom, const std::string& disc)
{
	display.panel = panel;
	ui::setDisplay(&display);
	st.rom = rom;
	st.disc = disc;
}

static void framePicture()
{
	display.panel->drain();
	if (!display.panel->readShown(st.frame, st.frameWidth, st.frameHeight))
		st.frame.clear();
}

bool open()
{
	if (ui::isOpen())
		return true;
	framePicture();
	st.previewSlot = -1;
	st.awaiting = -1;
	if (!ui::open(st.frame, st.frameWidth, st.frameHeight, ui::mainScreen(displayName(), host())))
	{
		st.frame.clear();
		return false;
	}
	return true;
}

bool openWithMessage(const std::string& text)
{
	if (!open())
		return false;
	ui::message(text);
	return true;
}

bool isOpen()
{
	return ui::isOpen();
}

void input(int code, bool pressed)
{
	if (!ui::isOpen())
		return;
	if (pressed && st.awaiting >= 0 && code != controls::MenuButton)
	{
		controls::bind((controls::Function)st.awaiting, code);
		st.awaiting = -1;
		ui::awaitControl(false);
		refreshBindings(ui::page());
		ui::move(ui::page(), 1);
		return;
	}
	// a recomp module's Controls, set from the emulator
	if (pressed && ui::awaitingControl())
	{
		ui::controlPressed(controls::inputName(code));
		return;
	}
	ui::button(menuButton(code), pressed);
}

Result frame()
{
	const ui::Result result = ui::frame();
	if (result == ui::Result::Open)
		return Result::Open;
	st.frame.clear();
	st.preview.clear();
	switch (result)
	{
	case ui::Result::Quit:
		return Result::Quit;
	case ui::Result::Switch:
		return Result::Switch;
	default:
		return Result::Resume;
	}
}

bool showStart()
{
	// shaders are compiled as the game first needs them (a stutter, once) and kept per game for its next start (saved when the
	// menu opens: GPU_Vulkan::SaveShaderCache)
	return ui::showStart("Loading...", fileName(st.rom), "First-time shader compilation may cause temporary stutter.\nCompiled shaders are then saved for re-use.");
}

bool starting()
{
	return ui::starting();
}

void endStart()
{
	ui::endStart();
}

void showStartError(const std::string& what)
{
	if (!ui::starting())
		return;
	ui::startLine("Cannot start " + displayName(), what);
	std::this_thread::sleep_for(std::chrono::seconds(3));
}

bool saveSlot()
{
	std::vector<u8> rgb;
	int w = 0, h = 0;
	display.panel->drain();
	if (!display.panel->readShown(rgb, w, h))
		rgb.clear();
	const bool ok = states::save(states::slotPath(st.slot), rgb, w, h);
	DS_LOG("state: slot %d %s", st.slot + 1, ok ? "saved" : "not saved");
	return ok;
}

bool slotExists()
{
	return states::exists(st.slot);
}

void openHostedControls(std::function<bool()> openPad, std::function<void()> closePad,
		std::function<std::vector<std::pair<int, bool>>()> events)
{
	// its own reading of the pad takes over: an arrow held is let go of, either way
	const auto letGo = [] {
		for (ui::Button arrow : { ui::Button::Up, ui::Button::Down, ui::Button::Left, ui::Button::Right })
			ui::button(arrow, false);
	};
	struct PadOnly
	{
		std::function<void()> close, letGo;
		~PadOnly()
		{
			close();
			letGo();
		}
	};
	letGo();
	const bool opened = openPad();
	auto padOnly = std::make_shared<PadOnly>(PadOnly { closePad, letGo });
	if (!opened)
	{
		ui::message("The pad could not be opened.");
		return;
	}
	st.awaiting = -1;
	Screen screen = bindingsScreen();
	screen.readsPad = true;
	screen.tick = [padOnly, events] {
		for (const auto& [code, pressed] : events())
			input(code, pressed);
	};
	ui::push(std::move(screen));
}

bool loadSlot()
{
	const bool ok = states::exists(st.slot) && states::load(states::slotPath(st.slot));
	DS_LOG("state: slot %d %s", st.slot + 1, ok ? "loaded" : "not loaded");
	return ok;
}

}
