#pragma once

// A message for the player, from the emulation: on the screen and in the log (core/deadstation/osd.cpp).

#include <string>
#include <string_view>

enum class OSDType {
	MESSAGE_INFO,
	MESSAGE_SUCCESS,
	MESSAGE_WARNING,
	MESSAGE_ERROR,
};

class OnScreenDisplay {
public:
	void Show(OSDType type, std::string_view text, float duration_s = 0.0f, const char *id = nullptr);
	void Show(OSDType type, std::string_view text, std::string_view text2, float duration_s = 0.0f, const char *id = nullptr);
	void CancelById(const char *id) {}
};

extern OnScreenDisplay g_OSD;
