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

#include <set>

#include "Core/CoreTiming.h"
#include "GPU/GeThread.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/MIPS/MIPS.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"

static std::set<CoreLifecycleFunc> lifecycleFuncs;

// This can be read and written from ANYWHERE.
volatile CoreState coreState = CORE_POWERDOWN;
static CoreState preGeCoreState = CORE_POWERDOWN;
// If true, core state has been changed, but JIT has probably not noticed yet.
volatile bool coreStatePending = false;

static BreakReason g_breakReason = BreakReason::None;

BreakReason Core_BreakReason() {
	return g_breakReason;
}

const char *CoreStateToString(CoreState state) {
	switch (state) {
	case CORE_RUNNING_CPU: return "RUNNING_CPU";
	case CORE_NEXTFRAME: return "NEXTFRAME";
	case CORE_POWERDOWN: return "POWERDOWN";
	case CORE_RUNTIME_ERROR: return "RUNTIME_ERROR";
	case CORE_RUNNING_GE: return "RUNNING_GE";
	}
	return "N/A";
}

void Core_ListenLifecycle(CoreLifecycleFunc func) {
	lifecycleFuncs.insert(func);
}

void Core_NotifyLifecycle(CoreLifecycle stage) {
	for (auto func : lifecycleFuncs)
		func(stage);
}

void Core_Stop() {
	Core_UpdateState(CORE_POWERDOWN);
}

void Core_UpdateState(CoreState newState) {
	if ((coreState == CORE_RUNNING_CPU || coreState == CORE_NEXTFRAME) && newState != CORE_RUNNING_CPU)
		coreStatePending = true;
	coreState = newState;
}

void Core_StateProcessed() {
	coreStatePending = false;
}

void Core_RunLoopUntil(u64 globalticks) {
	while (true) {
		switch (coreState) {
		case CORE_POWERDOWN:
		case CORE_RUNTIME_ERROR:
		case CORE_NEXTFRAME:
			return;
		case CORE_RUNNING_CPU:
			mipsr4k.RunLoopUntil(globalticks);
			break;  // Will loop around to go to RUNNING_GE or NEXTFRAME, which will exit.
		case CORE_RUNNING_GE:
			ge::runListsHere(CoreTiming::GetTicks());
			hleFinishSyscallAfterGe();
			coreState = preGeCoreState;
			break;
		}
	}
}

// Should only be called from GPUCommon functions (called from sceGe functions).
void Core_SwitchToGe() {
	preGeCoreState = coreState;
	coreState = CORE_RUNNING_GE;
}

void Core_Break(BreakReason reason, u32 relatedAddress) {
	ERROR_LOG(Log::CPU, "The game stopped: reason %d, address %08x", (int)reason, relatedAddress);
	g_breakReason = reason;
	Core_UpdateState(CORE_RUNTIME_ERROR);
}

// Should be called from the emulation thread.
bool Core_NextFrame() {
	if (coreState == CORE_RUNNING_CPU) {
		coreState = CORE_NEXTFRAME;
		return true;
	}
	ERROR_LOG(Log::System, "Core_NextFrame called with wrong core state %s", CoreStateToString(coreState));
	return false;
}

static const char *MemoryExceptionTypeAsString(MemoryExceptionType type) {
	switch (type) {
	case MemoryExceptionType::UNKNOWN: return "Unknown";
	case MemoryExceptionType::READ_WORD: return "Read Word";
	case MemoryExceptionType::WRITE_WORD: return "Write Word";
	case MemoryExceptionType::READ_BLOCK: return "Read Block";
	case MemoryExceptionType::WRITE_BLOCK: return "Read/Write Block";
	case MemoryExceptionType::ALIGNMENT: return "Alignment";
	default: return "N/A";
	}
}

// A bad access is the game's bug, as on a PSP: the game goes on, and it's in the log.
void Core_MemoryException(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type) {
	WARN_LOG(Log::MemMap, "%s: Invalid access at %08x (size %08x)", MemoryExceptionTypeAsString(type), address, accessSize);
}

void Core_MemoryExceptionInfo(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type, std::string_view additionalInfo, bool forceReport) {
	WARN_LOG(Log::MemMap, "%s: Invalid access at %08x (size %08x). %.*s", MemoryExceptionTypeAsString(type), address, accessSize, (int)additionalInfo.length(), additionalInfo.data());
}

// Can't be ignored
void Core_ExecException(u32 address, u32 pc, ExecExceptionType type) {
	WARN_LOG(Log::MemMap, "Invalid exec address %08x pc=%08x ra=%08x", address, pc, currentMIPS->r[MIPS_REG_RA]);
	Core_Break(BreakReason::CpuException, address);
}

void Core_BreakException(u32 pc) {
	ERROR_LOG(Log::CPU, "BREAK at %08x", pc);
}
