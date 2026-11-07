// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <cmath>

#include "Core/Config.h"

Config g_Config;

float Volume100ToMultiplier(int volume) {
	// Switch to linear above the 1.0f point.
	if (volume > 100) {
		return volume / 100.0f;
	}
	return powf(volume * 0.01f, 1.75f);
}
