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

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include "Core/MemMap.h"

void __DisplayInit();
void __DisplaySetPanelRate(double hz);
void __DisplayDoState(PointerWrap &p);
void __DisplayShutdown();

void Register_sceDisplay();

// Get information about the current framebuffer.
bool __DisplayGetFramebuf(PSPPointer<u8> *topaddr, u32 *linesize, u32 *pixelFormat, int mode);
void __DisplaySetFramebuf(u32 topaddr, int linesize, int pixelformat, int sync);

// Call this when resuming to avoid a small speedup burst
void __DisplaySetWasPaused();

void Register_sceDisplay_driver();
void __DisplayWaitForVblanks(const char* reason, int vblanks, bool callbacks = false);

// Kinda hacky, but what can you do... we can't pass it through the jit etc.
void ;

// the flip since the last call had to reach the screen, new picture or not
bool __DisplayTakeFlipForced();
// the flips so far, counted on the CPU thread (the GPU's own count, gpuStats.numFlips, follows its lists: ge::flipped)
int __DisplayFlips();

// Deadstation Portable: the sound queued for the speaker now and where it starts playing from, in seconds (the frontend's audio,
// as it queues more): Auto frame skip skips only while the queue is under that
void __DisplayNoteAudioQueued(float seconds, float startsAt);
// (tests) Auto frame skip's measure: the host seconds a second of the game's time took, drawn and skipped; whether it may skip now
void __DisplaySkipStats(float *drawn, float *skipped, bool *on);
void __DisplayTakeSkipCounts(int *skipped, int *waited, float *waitedMs, float *skipLateMeanMs, float *skipLateWorstMs);
