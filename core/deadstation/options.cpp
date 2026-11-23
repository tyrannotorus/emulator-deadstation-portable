// Deadstation Portable's settings, as Options shows them (ui/src/options.h): built into the shared UI as
// DEADUI_OPTIONS_SOURCE.
#include "options.h"

namespace deadui::options
{

const std::vector<Page>& pages()
{
	static const std::vector<Page> list {
		{ "Console", "PSP settings.", {
			{ "core", "Language", "Language", "Applies on restart.",
					{ { "Japanese", 0 }, { "English", 1 }, { "French", 2 }, { "Spanish", 3 }, { "German", 4 }, { "Italian", 5 },
					  { "Dutch", 6 }, { "Portuguese", 7 }, { "Russian", 8 }, { "Korean", 9 } }, false, 1 },
		} },
		{ "Emulator", "Deadstation Portable settings.", {
			{ "video", "ShowFPS", "Show FPS", "Frame rate overlay.", { { "Off", 0 }, { "On", 1 } }, true, 0 },
			{ "video", "Resolution", "Resolution", "Rendering resolution.", { { "480x272", 1 }, { "960x544", 2 } }, false, 1 },
			{ "video", "Picture", "Picture", "Picture shape.", { { "Original", 0 }, { "Fill", 1 }, { "Crop", 2 } }, false, 0 },
			{ "core", "CpuFreq", "CPU Freq", "The Brick's CPU speed.", { { "Dynamic", 0 }, { "1.8GHz", 1 } }, false, 1 },
		} },
	};
	return list;
}

}
