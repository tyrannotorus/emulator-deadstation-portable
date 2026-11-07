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

#include <mutex>
#include <vector>

#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HW/MemoryStick.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/MIPS.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"

namespace SaveState {

static int saveStateGeneration = 0;
static int saveDataGeneration = 0;
static std::string saveStateInitialGitVersion;

struct SaveStart {
	void DoState(PointerWrap &p);
};

void SaveStart::DoState(PointerWrap &p) {
	auto s = p.Section("SaveStart", 1, 3);
	if (!s)
		return;

	if (s >= 2) {
		// This only increments on save, of course.
		++saveStateGeneration;
		Do(p, saveStateGeneration);
		// This saves the first git version to create this save state (or generation of save states.)
		if (saveStateInitialGitVersion.empty())
			saveStateInitialGitVersion = PPSSPP_GIT_VERSION;
		Do(p, saveStateInitialGitVersion);
	} else {
		saveStateGeneration = 1;
	}
	if (s >= 3) {
		// Keep track of savedata (not save states) too.
		Do(p, saveDataGeneration);
	} else {
		saveDataGeneration = 0;
	}

	// Gotta do CoreTiming before HLE, but from v3 we've moved it after the memory stuff.
	if (s <= 2) {
		CoreTiming::DoState(p);
	}

	// Memory is a bit tricky when jit is enabled, since there's emuhacks in it.
	// These must be saved before copying out memory and restored after.
	auto savedReplacements = SaveAndClearReplacements();
	if (MIPSComp::jit && p.mode == p.MODE_WRITE) {
		std::lock_guard<std::recursive_mutex> guard(MIPSComp::jitLock);
		if (MIPSComp::jit) {
			std::vector<u32> savedBlocks;
			savedBlocks = MIPSComp::jit->SaveAndClearEmuHackOps();
			Memory::DoState(p);
			MIPSComp::jit->RestoreSavedEmuHackOps(savedBlocks);
		} else {
			Memory::DoState(p);
		}
	} else {
		Memory::DoState(p);
	}

	if (s >= 3) {
		CoreTiming::DoState(p);
	}

	// Don't bother restoring if reading, we'll deal with that in KernelModuleDoState.
	// In theory, different functions might have been runtime loaded in the state.
	if (p.mode != p.MODE_READ) {
		RestoreSavedReplacements(savedReplacements);
	}

	MemoryStick_DoState(p);
	currentMIPS->DoState(p);
	HLEDoState(p);
	__KernelDoState(p);
	// Kernel object destructors might close open files, so do the filesystem last.
	pspFileSystem.DoState(p);
}

void Init() {
	saveStateGeneration = 0;
	saveDataGeneration = 0;
	saveStateInitialGitVersion.clear();
}

void Shutdown() {
}

void NotifySaveData() {
	saveDataGeneration++;
}

bool Save(const Path &filename, std::string *error) {
	SaveStart state;
	std::string title = g_paramSFO.GetValueString("TITLE");
	if (title.empty()) {
		title = PSP_CoreParameter().fileToStart.GetFilename();
	}
	const CChunkFileReader::Error result = CChunkFileReader::Save(filename, title, PPSSPP_GIT_VERSION, state);
	if (result != CChunkFileReader::ERROR_NONE) {
		ERROR_LOG(Log::SaveState, "Save state failed: %d", (int)result);
		*error = "The state could not be saved";
		return false;
	}
	return true;
}

bool Load(const Path &filename, std::string *error) {
	SaveStart state;
	std::string version;
	const CChunkFileReader::Error result = CChunkFileReader::Load(filename, &version, state, error);
	if (result != CChunkFileReader::ERROR_NONE) {
		ERROR_LOG(Log::SaveState, "Load state failed: %d (%s)", (int)result, error->c_str());
		if (error->empty()) {
			*error = "The state could not be loaded";
		}
		return false;
	}
	return true;
}

}
