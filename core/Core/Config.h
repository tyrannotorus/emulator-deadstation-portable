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

// What the emulation reads, set by core/deadstation/settings.cpp from the player's settings. Defaults for the Brick.
#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
#include "Core/ConfigValues.h"
#include "Core/HLE/sceUtility.h"

// How the picture sits on the screen. The numbers are the saved setting's (ui/options/psp.cpp).
enum class PictureShape { Original = 0, Fill = 1, Crop = 2 };

struct DisplayLayoutConfig {
	int iDisplayFilter = SCALE_LINEAR;
	PictureShape picture = PictureShape::Original;
};

struct Config {
	// CPU
	int iCpuCore = (int)CPUCore::JIT;
	uint32_t uJitDisableFlags = 0;
	bool bFastMemory = true;
	bool bIgnoreBadMemAccess = true;
	bool bFuncReplacements = true;
	int iLockedCPUSpeed = 0;
	bool bSeparateSASThread = true;
	int iIOTimingMethod = IOTIMING_FAST;
	int iForceEnableHLE = 0;
	int iDisableHLE = 0;

	// Graphics
	int iInternalResolution = 1;
	bool bHardwareTransform = true;
	// Deadstation Portable: God of War's blur and glow (its own option, settings.cpp); false: none of it drawn (DrawEngineVulkan)
	bool bMotionBlur = true;
	// Deadstation Portable: God of War's frame rate (its own option, settings.cpp): 60 (upstream's GoWFramerateHack60) or 30
	int iGodOfWarFrameRate = 60;
	bool bSoftwareSkinning = true;
	bool bVertexDecoderJit = true;
	bool bSkipBufferEffects = false;
	int iSkipGPUReadbackMode = (int)SkipGPUReadbackMode::NO_SKIP;
	bool bTextureBackoffCache = false;
	int iTexFiltering = TEX_FILTER_AUTO;
	int iAnisotropyLevel = 0;
	int iMultiSampleLevel = 0;
	int bHighQualityDepth = 1;
	int iBloomHack = 0;
	int iSplineBezierQuality = 2;
	int iDepthRasterMode = (int)DepthRasterMode::OFF;
	bool bUberShaderVertex = true;
	bool bUberShaderFragment = true;
	bool bShaderCache = true;
	int iFrameSkip = 0;
	bool bAutoFrameSkip = false;
	DisplayLayoutConfig displayLayout;

	const DisplayLayoutConfig &GetDisplayLayoutConfig() const { return displayLayout; }

	// Sound
	bool bEnableSound = true;
	int iGameVolume = VOLUME_FULL;
	int iReverbVolume = VOLUME_FULL;

	// The PSP
	int iPSPModel = PSP_MODEL_SLIM;
	int iFirmwareVersion = PSP_DEFAULT_FIRMWARE;
	int iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	int iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
	int iDateFormat = PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD;
	int iTimeFormat = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
	int iTimeZone = 0;
	bool bDayLightSavings = false;
	int iLockParentalLevel = 0;
	bool bEncryptSave = true;
	bool bMemStickInserted = true;
	int iMemStickSizeGB = 16;
	std::string sNickName = "Deadstation";
	std::string sMACAddress = "02:00:00:00:00:01";
	std::string sLanguageIni = "en_US";

	// Ad-hoc, between two Bricks: comes with the link
	bool bEnableWlan = false;
	bool bEnableAdhocServer = false;
	int iPortOffset = 10000;
	int iMinTimeout = 0;
	int iWlanAdhocChannel = PSP_SYSTEMPARAM_ADHOC_CHANNEL_AUTOMATIC;
	std::string sProAdhocServer;

	// Save states
	int iCurrentStateSlot = 0;
	int iSaveStateSlotCount = 10;

	// Where things are
	Path memStickDirectory;
	Path flash0Directory;
	Path appCacheDirectory;
};

extern Config g_Config;
