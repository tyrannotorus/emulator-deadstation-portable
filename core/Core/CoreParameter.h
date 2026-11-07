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

#pragma once

#include <string>

#include "Common/File/Path.h"
#include "Core/Compatibility.h"
#include "Core/Loaders.h"

class FileLoader;
class GraphicsContext;
enum class CPUCore;

// PSP_CoreParameter()
struct CoreParameter {
	CPUCore cpuCore;
	GraphicsContext *graphicsContext = nullptr;
	bool enableSound = true;  // there aren't multiple sound cores.

	Path fileToStart;
	std::string errorString;

	// Internal PSP rendering resolution and scale factor.
	int renderScaleFactor = 1;
	int renderWidth = 0;
	int renderHeight = 0;

	// Actual output resolution in pixels.
	int pixelWidth = 0;
	int pixelHeight = 0;

	IdentifiedFileType fileType = IdentifiedFileType::UNKNOWN;

	Compatibility compat;
};
