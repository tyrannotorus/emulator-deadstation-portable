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
#include <thread>

#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/GraphicsContext.h"
#include "Common/StringUtils.h"
#include "Common/ExceptionHandlerSetup.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HW/Display.h"
#include "Core/Loaders.h"
#include "Core/MemFault.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/PSPLoaders.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "GPU/GPUCommon.h"

MetaFileSystem pspFileSystem;
ParamSFOData g_paramSFO;
ParamSFOData g_paramSFORaw;
CoreParameter g_CoreParameter;
static FileLoader *g_loadedFile;
// For background loading thread.
static std::thread g_loadingThread;

bool coreCollectDebugStats = false;

static BootState g_bootState = BootState::Off;

BootState PSP_GetBootState() {
	return g_bootState;
}

FileLoader *PSP_LoadedFile() {
	return g_loadedFile;
}

void UpdateLoadedFile(FileLoader *fileLoader) {
	delete g_loadedFile;
	g_loadedFile = fileLoader;
}

void CPU_Shutdown();

// NOTE: The loader has already been fully resolved (ResolveFileLoaderTarget) and identified here.
static bool CPU_Init(FileLoader *fileLoader, IdentifiedFileType type, std::string *errorString) {
	// Default memory settings
	// Seems to be the safest place currently..
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE; // 32 MB of ram by default

	Memory::g_PSPModel = g_Config.iPSPModel;

	g_CoreParameter.fileType = type;

	std::string gameTitle = "Unidentified PSP title";

	switch (type) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:
		// Doesn't seem to take ownership of fileLoader?
		if (!MountGameISO(fileLoader, errorString)) {
			*errorString = "Failed to mount ISO file: " + *errorString;
			return false;
		}
		if (LoadParamSFOFromDisc()) {
			InitMemorySizeForGame();
		}
		break;
	case IdentifiedFileType::PSP_PBP:
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
		// This is normal for homebrew.
		if (LoadParamSFOFromPBP(fileLoader)) {
			InitMemorySizeForGame();
		}
		break;
	case IdentifiedFileType::PSP_ELF:
		if (Memory::g_PSPModel != PSP_MODEL_FAT) {
			INFO_LOG(Log::Loader, "ELF, using full PSP-2000 memory access");
			Memory::g_MemorySize = Memory::RAM_DOUBLE_SIZE;
		}
		break;
	default:
		ERROR_LOG(Log::Loader, "Not a PSP game. %s", errorString->c_str());
		if (errorString->empty()) {
			*errorString = "Not a PSP game";
		}
		return false;
	}

	// OK, pretty confident we have a PSP game.
	if (g_paramSFO.IsValid()) {
		gameTitle = SanitizeString(g_paramSFO.GetValueString("TITLE"), StringRestriction::NoLineBreaksOrSpecials);
	}
	INFO_LOG(Log::Loader, "%s : %s", g_paramSFO.GetValueString("DISC_ID").c_str(), gameTitle.c_str());

	_dbg_assert_(!g_symbolMap);
	g_symbolMap = new SymbolMap();

	MIPSAnalyst::Reset();
	Replacement_Init();

	// Here we have read the PARAM.SFO, let's see if we need any compatibility overrides.
	// Homebrew get fake disc IDs assigned to the global paramSFO, so they shouldn't clash with real games.
	std::string discId = g_paramSFO.GetDiscID();
	g_CoreParameter.compat.Load(discId);

	Memory::MemMapSetupFlags memMapFlags = Memory::MemMapSetupFlags::Default;
	if (g_CoreParameter.compat.flags().NullPageValid) {
		memMapFlags = Memory::MemMapSetupFlags::AllocNullPage;
	}

	// Initialize the memory map as early as possible (now that we've read the PARAM.SFO).
	if (!Memory::Init(memMapFlags)) {
		// We're screwed.
		*errorString = "Memory init failed";
		return false;
	}

	InitVFPU();

	mipsr4k.Reset();

	CoreTiming::Init();

	DisplayHWInit();

	// Init all the HLE modules
	HLEInit();

	g_loadedFile = fileLoader;

	switch (type) {
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
	{
		std::string dir = fileLoader->GetPath().GetDirectory();
		size_t pos = dir.find("PSP/GAME/");
		if (pos != std::string::npos) {
			dir = ResolvePBPDirectory(Path(dir)).ToString();
			pspFileSystem.SetStartingDirectory("ms0:/" + dir.substr(pos));
		}
		if (!Load_PSP_ELF_PBP(fileLoader, discId, errorString)) {
			return false;
		}
		break;
	}
	// Looks like a wrong fall through but is not, both paths are handled above.

	case IdentifiedFileType::PSP_PBP:
	case IdentifiedFileType::PSP_ELF:
	{
		INFO_LOG(Log::Loader, "File is an ELF or loose PBP %s", fileLoader->GetPath().c_str());
		if (!Load_PSP_ELF_PBP(fileLoader, discId, errorString)) {
			ERROR_LOG(Log::Loader, "Failed to load ELF or loose PBP: %s", errorString->c_str());
			return false;
		}
		break;
	}

	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:	// behaves the same as the mounting is already done by now
		pspFileSystem.SetStartingDirectory("disc0:/PSP_GAME/USRDIR");
		if (!Load_PSP_ISO(fileLoader, errorString)) {
			return false;
		}
		break;

	default:
		*errorString = "Not a PSP game";
		g_CoreParameter.fileToStart.clear();
		return false;
	}

	InstallExceptionHandler(&Memory::HandleFault);

	return true;
}

void CPU_Shutdown() {
	UninstallExceptionHandler();

	Replacement_Shutdown();

	CoreTiming::Shutdown();
	__KernelShutdown();
	HLEShutdown();

	DisplayHWShutdown();

	pspFileSystem.Shutdown();
	mipsr4k.Shutdown();
	Memory::Shutdown();

	delete g_loadedFile;
	g_loadedFile = nullptr;

	delete g_symbolMap;
	g_symbolMap = nullptr;
}

static void InitGPU(std::string *error_string) {
	if (!gpu) {  // should be!
		INFO_LOG(Log::Loader, "Starting graphics...");
		Draw::DrawContext *draw = g_CoreParameter.graphicsContext ? g_CoreParameter.graphicsContext->GetDrawContext() : nullptr;
		// This set the `gpu` global.
		if (!GPU_Init(g_CoreParameter.graphicsContext, draw)) {
			*error_string = "Unable to initialize rendering engine.";
			CPU_Shutdown();
			g_bootState = BootState::Failed;
		}
	}
}

bool PSP_InitStart(const CoreParameter &coreParam) {
	if (g_bootState != BootState::Off) {
		ERROR_LOG(Log::Loader, "Can't start loader thread - already on.");
		return false;
	}

	g_bootState = BootState::Booting;

	GraphicsContext *temp = g_CoreParameter.graphicsContext;
	g_CoreParameter = coreParam;
	if (g_CoreParameter.graphicsContext == nullptr) {
		g_CoreParameter.graphicsContext = temp;
	}

	g_CoreParameter.errorString.clear();

	std::string *errorString = &g_CoreParameter.errorString;

	INFO_LOG(Log::Loader, "Starting loader thread...");

	_assert_msg_(!g_loadingThread.joinable(), "%s", coreParam.fileToStart.c_str());

	Core_NotifyLifecycle(CoreLifecycle::STARTING);

	g_loadingThread = std::thread([errorString]() {
		SetCurrentThreadName("dsp-loader");

		Path filename = g_CoreParameter.fileToStart;

		IdentifiedFileType fileType;
		FileLoader *loadedFile = ResolveFileLoaderTarget(ConstructFileLoader(filename), &fileType, errorString);

		g_CoreParameter.fileType = fileType;

		// TODO: The reason we pass in g_CoreParameter.errorString here is that it's persistent -
		// it gets written to from the loader thread that gets spawned.
		if (!CPU_Init(loadedFile, fileType, &g_CoreParameter.errorString)) {
			CPU_Shutdown();
			g_CoreParameter.fileToStart.clear();
			*errorString = g_CoreParameter.errorString;
			if (errorString->empty()) {
				*errorString = "Failed initializing CPU/Memory";
			}
			g_bootState = BootState::Failed;
			return;
		}

		// Initialize the GPU as far as we can here (do things like load cache files).
		_dbg_assert_(!gpu);
		InitGPU(errorString);
		if (g_bootState != BootState::Failed)
			g_bootState = BootState::Complete;
	});

	return true;
}

BootState PSP_InitUpdate(std::string *error_string) {
	const BootState bootState = g_bootState;

	if (bootState == BootState::Booting || bootState == BootState::Off) {
		// Nothing to do right now.
		return bootState;
	}

	// Since we load on a background thread, wait for startup to complete.
	_assert_msg_(g_loadingThread.joinable(), "bootstate: %d", (int)bootState);
	g_loadingThread.join();

	if (bootState == BootState::Failed) {
		// Failed! (Note: the CPU was already shut down on the loader thread).
		Core_NotifyLifecycle(CoreLifecycle::START_COMPLETE);
		*error_string = g_CoreParameter.errorString;
		g_bootState = BootState::Off;
		return BootState::Failed;
	}

	// Ok, async part of the boot completed, let's finish up things on the main thread.
	gpu->FinishInitOnMainThread();

	Core_NotifyLifecycle(CoreLifecycle::START_COMPLETE);
	return BootState::Complete;
}

void PSP_Shutdown() {
	// Do nothing if we never inited.
	if (g_bootState == BootState::Off) {
		ERROR_LOG(Log::Loader, "Unexpected PSP_Shutdown");
		return;
	}

	Core_Stop();

	Core_NotifyLifecycle(CoreLifecycle::STOPPING);

	CPU_Shutdown();
	GPU_Shutdown();
	g_paramSFO.Clear();
	g_paramSFORaw.Clear();

	currentMIPS = nullptr;

	Core_NotifyLifecycle(CoreLifecycle::STOPPED);

	g_bootState = BootState::Off;
}

void PSP_RunLoopWhileState() {
	// We just run the CPU until we get to vblank. This will quickly sync up pretty nicely.
	// The actual number of cycles doesn't matter so much here as we will break due to CORE_NEXTFRAME, most of the time hopefully...
	int blockTicks = usToCycles(1000000 / 10);
	// Run until CORE_NEXTFRAME
	Core_RunLoopUntil(CoreTiming::GetTicks() + blockTicks);
}
