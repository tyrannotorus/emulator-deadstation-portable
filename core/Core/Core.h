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

// Why the core stopped.
enum class BreakReason {
	None,
	MemoryException,
	CpuException,
	BreakInstruction,
	SavestateCrash,
};

// The game can't go on: the run loop returns, and the frontend ends it.
void Core_Break(BreakReason reason, u32 relatedAddress = 0);
BreakReason Core_BreakReason();

bool Core_NextFrame();
void Core_SwitchToGe();  // Switches from CPU emulation to GE display list execution.

enum class CoreLifecycle {
	STARTING,
	// Note: includes failure cases.  Guaranteed call after STARTING.
	START_COMPLETE,
	STOPPING,
	// Guaranteed call after STOPPING.
	STOPPED,

	// Sometimes called for save states.  Guaranteed sequence, and never during STARTING or STOPPING.
	MEMORY_REINITING,
	MEMORY_REINITED,
};

// RUNNING must be at 0, NEXTFRAME must be at 1.
enum CoreState {
	// Emulation is running normally.
	CORE_RUNNING_CPU = 0,
	// Emulation was running normally, just reached the end of a frame.
	CORE_NEXTFRAME = 1,
	// Core is not running.
	CORE_POWERDOWN,
	// Unrecoverable runtime error.
	CORE_RUNTIME_ERROR,
	// Running the GPU. When done, will switch over to RUNNING_CPU.
	CORE_RUNNING_GE,
};
const char *CoreStateToString(CoreState state);

// Callback is called on the Emu thread.
typedef void (* CoreLifecycleFunc)(CoreLifecycle stage);
void Core_ListenLifecycle(CoreLifecycleFunc func);
void Core_NotifyLifecycle(CoreLifecycle stage);

void Core_Stop();
void Core_RunLoopUntil(u64 globalticks);

extern volatile CoreState coreState;
extern volatile bool coreStatePending;

void Core_UpdateState(CoreState newState);
void Core_StateProcessed();

enum class MemoryExceptionType {
	NONE,
	UNKNOWN,
	READ_WORD,
	WRITE_WORD,
	READ_BLOCK,
	WRITE_BLOCK,
	ALIGNMENT,
};
enum class ExecExceptionType {
	JUMP,
	THREAD,
};

// Separate one for without info, to avoid having to allocate a string
void Core_MemoryException(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type);
void Core_MemoryExceptionInfo(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type, std::string_view additionalInfo, bool forceReport);
void Core_ExecException(u32 address, u32 pc, ExecExceptionType type);
void Core_BreakException(u32 pc);
