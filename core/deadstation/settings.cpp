#include "settings.h"
#include "clock.h"
#include "log.h"

#include "Core/Config.h"
#include "GPU/GeThread.h"

#include <cstdlib>
#include <cstring>

namespace deadstation::settings
{
namespace
{

unsigned pending = 0;

// God of War: Chains of Olympus, every region and demo compat.ini knows
bool isChainsOfOlympus(const std::string& id)
{
	static const char *const ids[] = { "UCAS40198", "UCUS98653", "UCES00842", "ULJM05438", "ULJM05348", "UCKS45084", "NPUG80325",
		"NPEG00023", "NPHG00027", "NPHG00028", "NPJH50170", "UCET00844", "UCUS98705", "UCED00971", "UCUS98713" };
	for (const char *g : ids)
		if (id == g)
			return true;
	return false;
}

const options::Setting *findSetting(const char *section, const char *key)
{
	for (const options::Page& page : options::gamePages())
		for (const options::Setting& s : page.settings)
			if (strcmp(s.section, section) == 0 && strcmp(s.key, key) == 0)
				return &s;
	return nullptr;
}

// a row of Options' pages (options.cpp, and the game's own below) by its section and key: a key no row has is a mistake
// here, said and stopped
const options::Setting& row(const char *section, const char *key)
{
	if (const options::Setting *s = findSetting(section, key))
		return *s;
	DS_LOG("settings: no row [%s] %s", section, key);
	std::abort();
}

// a row's value as the game has it (else the defaults, else the row's own default)
int fileValue(const char *section, const char *key)
{
	return options::game().get(row(section, key));
}

bool chainsOfOlympus = false;	// the game played has God of War's own rows

}	// namespace

void setGame(const std::string& discId)
{
	options::setGame(discId);
	chainsOfOlympus = isChainsOfOlympus(discId);
	if (chainsOfOlympus)
		options::addGameSettings("Emulator", {
			{ "game", "FrameRate", "Frame Rate", "The game's frame rate.", { { "30", 30 }, { "60", 60 } }, false, 60 },
			{ "game", "MotionBlur", "Motion Blur", "The game's blur.", { { "Off", 0 }, { "On", 1 } }, false, 0 },
		});
}

void apply()
{
	g_Config.iLanguage = fileValue("core", "Language");
	cpufreq::setFixed(fileValue("core", "CpuFreq") != 0);
	g_Config.iInternalResolution = resolution();
	int picture = fileValue("video", "Picture");
	// tests: DEADSTATION_PORTABLE_PICTURE=0, 1 or 2, over the files
	if (const char *test = getenv("DEADSTATION_PORTABLE_PICTURE"); test != nullptr && *test != '\0')
		picture = atoi(test);
	g_Config.displayLayout.picture = picture == 1 ? PictureShape::Fill : picture == 2 ? PictureShape::Crop : PictureShape::Original;
	// Frame Skip always Auto (no row, as in Deadcast and Deadstation): frames skipped only while the game is behind and the sound
	// running low, and only while skipping is measured to pay (sceDisplay.cpp)
	int frameSkip = 1;
	// tests: DEADSTATION_PORTABLE_FRAMESKIP=0 or 1
	if (const char *test = getenv("DEADSTATION_PORTABLE_FRAMESKIP"); test != nullptr && *test != '\0')
		frameSkip = atoi(test);
	const bool autoSkip = frameSkip != 0;
	g_Config.bAutoFrameSkip = autoSkip;
	g_Config.iFrameSkip = autoSkip ? 3 : 0;
	// textures sampled as the game asks, as the PSP does
	g_Config.iTexFiltering = TEX_FILTER_AUTO;
	// the game's own settings: God of War's Frame Rate and Motion Blur; any other game plays at 60 with its blur as it draws it
	g_Config.iGodOfWarFrameRate = chainsOfOlympus && fileValue("game", "FrameRate") == 30 ? 30 : 60;
	g_Config.bMotionBlur = chainsOfOlympus ? fileValue("game", "MotionBlur") != 0 : true;
	// tests: DEADSTATION_PORTABLE_GELATE=0 or 1, GE interrupts always exact or always when the GE's thread gets to them, not chosen
	// by measurement (GPU/GeThread.h)
	if (const char *late = getenv("DEADSTATION_PORTABLE_GELATE"); late != nullptr && *late != '\0') {
		ge::lateMode = atoi(late) != 0 ? 1 : 0;
		ge::lateEvents = ge::lateMode == 1;
	}
}

bool showFps()
{
	return fileValue("video", "ShowFPS") != 0;
}

int resolution()
{
	// tests: DEADSTATION_PORTABLE_RESOLUTION=1 or 2, over the files
	if (const char *test = getenv("DEADSTATION_PORTABLE_RESOLUTION"); test != nullptr && *test != '\0')
		return atoi(test) == 2 ? 2 : 1;
	return fileValue("video", "Resolution") == 2 ? 2 : 1;
}

options::Store store()
{
	options::Store game = options::game();
	return {
		game.get,
		[game](const options::Setting& setting, int v) {
			game.set(setting, v);
			pending |= setting.key == row("video", "Resolution").key ? Resolution : Other;
		},
	};
}

unsigned changed()
{
	const unsigned c = pending;
	pending = 0;
	return c;
}

}
