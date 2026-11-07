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

#include <string_view>
#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
#include "Core/CoreParameter.h"
#include "Core/ConfigValues.h"
#include "Core/Util/PathUtil.h"

class MetaFileSystem;
class ParamSFOData;

extern MetaFileSystem pspFileSystem;
extern ParamSFOData g_paramSFO;
extern ParamSFOData g_paramSFORaw;

enum class BootState {
	Off,
	Booting,
	Complete,
	Failed,
};

BootState PSP_GetBootState();
inline bool PSP_IsInited() {
	return PSP_GetBootState() == BootState::Complete;
}

// Call this once, then call PSP_InitUpdate repeatedly to monitor progress.
bool PSP_InitStart(const CoreParameter &coreParam);

// Check the return value of this - if Booting, keep calling.
// If Complete or Failed, handle as appropriate, and stop calling.
BootState PSP_InitUpdate(std::string *error_string);

void PSP_Shutdown();

FileLoader *PSP_LoadedFile();

void PSP_RunLoopWhileState();

void UpdateLoadedFile(FileLoader *fileLoader);

// NOTE: These are almost all derived from g_Config.memStickDirectory directly -
// they are not stored anywhere.
Path GetSysDirectory(PSPDirectories directoryType);

extern bool coreCollectDebugStats;

inline CoreParameter &PSP_CoreParameter() {
	extern CoreParameter g_CoreParameter;
	return g_CoreParameter;
}
