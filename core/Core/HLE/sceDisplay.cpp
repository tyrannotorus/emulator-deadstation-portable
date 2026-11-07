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

#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

// TODO: Move this somewhere else, cleanup.
#include <unistd.h>
#include <sys/time.h>

#include "Common/Data/Text/I18n.h"
#include "Common/System/System.h"
#include "Common/System/OSD.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeMap.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/CoreParameter.h"
#include "Core/FrameTiming.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceDisplay.h"
#include "GPU/GeThread.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceNet.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HW/Display.h"
#include "Core/Util/PPGeDraw.h"

#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"

struct FrameBufferState {
	u32 topaddr;
	GEBufferFormat fmt;
	int stride;
};

struct WaitVBlankInfo {
	WaitVBlankInfo(u32 tid) : threadID(tid), vcountUnblock(1) {}
	WaitVBlankInfo(u32 tid, int vcount) : threadID(tid), vcountUnblock(vcount) {}
	SceUID threadID;
	// Number of vcounts to block for.
	int vcountUnblock;

	void DoState(PointerWrap &p) {
		auto s = p.Section("WaitVBlankInfo", 1);
		if (!s)
			return;

		Do(p, threadID);
		Do(p, vcountUnblock);
	}
};

// STATE BEGIN
static FrameBufferState framebuf;
static FrameBufferState latchedFramebuf;
static bool framebufIsLatched;

static int enterVblankEvent = -1;
static int leaveVblankEvent = -1;
static int afterFlipEvent = -1;


static int numSkippedFrames;
static bool hasSetMode;
static int resumeMode;
static int holdMode;
static int brightnessLevel;
static int mode;
static int width;
static int height;
static bool wasPaused;
static bool flippedThisFrame;

static int framerate;
// One game frame a vblank of the panel, at its rate (the PSP is 59.94, the panel as near as its timing goes)
static double panelRefreshHz = 60000.0 / 1001.0;

// 1.001f to compensate for the classic 59.94 NTSC framerate that the PSP seems to have.
static double timePerVblank;

// Don't include this in the state, time increases regardless of state.
static double curFrameTime;
static double lastFrameTime;
static double nextFrameTime;
static int numVBlanksSinceFlip;

const int PSP_DISPLAY_MODE_LCD = 0;

std::vector<WaitVBlankInfo> vblankWaitingThreads;
// Key is the callback id it was for, or if no callback, the thread id.
// Value is the goal vcount number (in case the callback takes >= 1 vcount to return.)
std::map<SceUID, int> vblankPausedWaits;

// STATE END

// The vblank period is 731.5 us (0.7315 ms)
const double vblankMs = 0.7315;
// These are guesses based on tests.
const double vsyncStartMs = 0.5925;
const double vsyncEndMs = 0.7265;
double frameMs;

enum {
	PSP_DISPLAY_SETBUF_IMMEDIATE = 0,
	PSP_DISPLAY_SETBUF_NEXTFRAME = 1
};

// For the "max 60 fps" setting.
static int lastFlipsTooFrequent = 0;
static u64 lastFlipCycles = 0;
static u64 nextFlipCycles = 0;

void hleEnterVblank(u64 userdata, int cyclesLate);
void hleLeaveVblank(u64 userdata, int cyclesLate);
void hleAfterFlip(u64 userdata, int cyclesLate);

void __DisplayVblankBeginCallback(SceUID threadID, SceUID prevCallbackId);
void __DisplayVblankEndCallback(SceUID threadID, SceUID prevCallbackId);

void __DisplayFlip(int cyclesLate);
static void __DisplaySetFramerate(void);

// Deadstation Portable: Auto frame skip (DoFrameTiming).
// The sound queued for the speaker (__DisplayNoteAudioQueued): under where it starts playing, the game is losing ground the player
// is about to hear.
static std::atomic<float> audioQueued { 1.0f }, audioStartsAt { 0.0f };

void __DisplayNoteAudioQueued(float seconds, float startsAt) {
	audioQueued.store(seconds, std::memory_order_relaxed);
	audioStartsAt.store(startsAt, std::memory_order_relaxed);
}

static bool AudioRunningLow() {
	return audioQueued.load(std::memory_order_relaxed) < audioStartsAt.load(std::memory_order_relaxed);
}

// What a skipped frame saves, measured: the host time a frame of the game took, per second of the game's time, drawn and skipped.
// Only frames that didn't wait for real time to catch up count: a frame's time is then what it cost. Skipped frames that cost as
// much as drawn ones (the game's own CPU work holding it back) save nothing: skipping then only shows fewer frames, so it stops,
// and is measured again a while later.
static double skipLastFlip = 0.0;
static bool skipLastWaited = true;
static bool skipLastSkipped = false;
static double costDrawn = 0.0, costSkipped = 0.0;
static int drawnMeasured = 0, skippedMeasured = 0;
static double skipOffUntil = 0.0;
// Flips in a row each within 4 vblanks of the last, since a pause, a state loaded or a stretch without flips (a load, which then
// runs unthrottled: LEGO's intro, after its state, ran 37 s of its time in a second). Skipping only once 60 in a row: what a frame
// is skipped for is the game's steady play, and a load whose pace depends on the time the game sees can stall when frames are
// skipped then (LEGO's intro, once in three, stopped loading and never drew again).
static int steadyFlips = 0;

static void SkipMeasure(double now, float scaledTimestep, bool paused) {
	if (paused || scaledTimestep > 4.0f * (float)timePerVblank)
		steadyFlips = 0;
	else if (scaledTimestep > 0.f)
		steadyFlips++;
	if (!paused && skipLastFlip != 0.0 && !skipLastWaited && scaledTimestep > 0.f) {
		const double cost = (now - skipLastFlip) / scaledTimestep;
		// over 4 times the game's time: a pause, a load, not a frame's work
		if (cost < 4.0) {
			if (skipLastSkipped) {
				costSkipped = skippedMeasured++ == 0 ? cost : costSkipped * 0.8 + cost * 0.2;
			} else {
				costDrawn = drawnMeasured++ == 0 ? cost : costDrawn * 0.9 + cost * 0.1;
			}
		}
	}
	skipLastFlip = now;
}

// (perf) since the last ask: frames skipped, frames that waited for real time and how long, frames skipped while the sound
// queued was low, and the worst lateness a skip was decided at
static int statSkipped = 0, statWaited = 0;
static double statWaitedFor = 0.0, statSkipLateWorst = 0.0, statSkipLateSum = 0.0;

void __DisplaySkipStats(float *drawn, float *skipped, bool *on) {
	*drawn = (float)costDrawn;
	*skipped = (float)costSkipped;
	*on = steadyFlips >= 60 && time_now_d() >= skipOffUntil;
}

void __DisplayTakeSkipCounts(int *skipped, int *waited, float *waitedMs, float *skipLateMeanMs, float *skipLateWorstMs) {
	*skipped = statSkipped;
	*waited = statWaited;
	*waitedMs = (float)(statWaitedFor * 1000.0);
	*skipLateMeanMs = statSkipped > 0 ? (float)(statSkipLateSum / statSkipped * 1000.0) : 0.f;
	*skipLateWorstMs = (float)(statSkipLateWorst * 1000.0);
	statSkipped = statWaited = 0;
	statWaitedFor = statSkipLateWorst = statSkipLateSum = 0.0;
}

static bool SkipPays(double now) {
	if (steadyFlips < 60 || now < skipOffUntil)
		return false;
	// still finding out
	if (skippedMeasured < 12 || drawnMeasured < 12)
		return true;
	if (costSkipped < costDrawn * 0.97)
		return true;
	// not 3% cheaper: off for 2 s, then measured again
	skipOffUntil = now + 2.0;
	skippedMeasured = 0;
	return false;
}

// GE interrupts exact or when the GE's thread gets to them (ge::lateEvents), by measurement: in the game's steady play each runs a
// stretch, and the one whose frames cost less host time a vblank is kept for a while, then measured again. A frame's cost: the
// host time since the last flip, less what that flip waited for real time. Late is kept only by a clear margin: exact is what the game would see on a PSP, and its serial work is what NextUI's
// Auto clock sees (LEGO and OutRun: about 1 vblank/s faster late; God of War 0.8 slower; Tekken 6 the same).
static int geModePhase = 0;			// 0 holding, 1 measuring exact, 2 measuring late
static int geModeFrames = 0;
static double geModeCost[2] = {};
static int geModeCounted[2] = {};
static double geModeNextAt = 0.0;
static double geModeLastFlip = 0.0, geModeLastWait = 0.0;
static bool geModeChoseLate = false;

static void GeModeSet(bool late) {
	if (ge::lateEvents == late)
		return;
	ge::sync();
	ge::lateEvents = late;
}

// now: this flip's time; wait: what it will wait for real time; vblanks: the game's time since the last flip
static void GeModeMeasure(double now, double wait, double vblanks, bool steady, bool skipped) {
	if (ge::lateMode >= 0)
		return;
	const double cost = geModeLastFlip != 0.0 && vblanks > 0.0 ? (now - geModeLastFlip - geModeLastWait) / vblanks : -1.0;
	const bool counts = cost >= 0.0 && cost < 0.1 && !skipped;
	geModeLastFlip = now;
	geModeLastWait = wait;
	constexpr int Settle = 20, Frames = 150;
	// late kept only 0.3 ms a vblank cheaper
	constexpr double Margin = 0.0003, Hold = 60.0;
	if (!steady) {
		// a load or a pause: what was measured isn't play
		if (geModePhase != 0) {
			geModePhase = 0;
			geModeNextAt = now + 2.0;
			GeModeSet(geModeChoseLate);
		}
		return;
	}
	if (geModePhase == 0) {
		if (now < geModeNextAt)
			return;
		geModePhase = 1;
		geModeFrames = 0;
		geModeCost[0] = geModeCost[1] = 0.0;
		geModeCounted[0] = geModeCounted[1] = 0;
		GeModeSet(false);
		return;
	}
	const int m = geModePhase - 1;
	if (++geModeFrames > Settle && counts) {
		geModeCost[m] += cost;
		geModeCounted[m]++;
	}
	if (geModeFrames < Frames)
		return;
	if (geModePhase == 1) {
		geModePhase = 2;
		geModeFrames = 0;
		GeModeSet(true);
		return;
	}
	const double exact = geModeCounted[0] > 0 ? geModeCost[0] / geModeCounted[0] : 0.0;
	const double late = geModeCounted[1] > 0 ? geModeCost[1] / geModeCounted[1] : 0.0;
	// clearly cheaper one way or the other; otherwise as it was (a game holding 60 either way costs a vblank a vblank in both)
	const bool measured = geModeCounted[0] >= Frames / 2 && geModeCounted[1] >= Frames / 2;
	bool keepLate = geModeChoseLate;
	if (measured && late < exact - Margin)
		keepLate = true;
	else if (measured && exact < late - Margin)
		keepLate = false;
	NOTICE_LOG(Log::sceDisplay, "GE interrupts %s: %.2f ms a vblank exact, %.2f ms late (%d, %d frames)", keepLate ? "late" : "exact", exact * 1000.0, late * 1000.0, geModeCounted[0], geModeCounted[1]);
	geModeChoseLate = keepLate;
	GeModeSet(keepLate);
	geModePhase = 0;
	geModeNextAt = now + Hold;
}

static bool UseAutoFrameSkip() {
	return g_Config.bAutoFrameSkip && !g_Config.bSkipBufferEffects;
}



void __DisplayInit() {
	__DisplaySetFramerate();
	hasSetMode = false;
	mode = 0;
	resumeMode = 0;
	holdMode = 0;
	brightnessLevel = 84;
	width = 480;
	height = 272;
	numSkippedFrames = 0;
	numVBlanksSinceFlip = 0;
	flippedThisFrame = false;
	framebufIsLatched = false;
	framebuf.topaddr = 0x04000000;
	framebuf.fmt = GE_FORMAT_8888;
	framebuf.stride = 512;
	memcpy(&latchedFramebuf, &framebuf, sizeof(latchedFramebuf));
	lastFlipsTooFrequent = 0;
	lastFlipCycles = 0;
	nextFlipCycles = 0;
	wasPaused = false;

	enterVblankEvent = CoreTiming::RegisterEvent("EnterVBlank", &hleEnterVblank);
	leaveVblankEvent = CoreTiming::RegisterEvent("LeaveVBlank", &hleLeaveVblank);
	afterFlipEvent = CoreTiming::RegisterEvent("AfterFlip", &hleAfterFlip);


	CoreTiming::ScheduleEvent(msToCycles(frameMs - vblankMs), enterVblankEvent, 0);
	curFrameTime = 0.0;
	nextFrameTime = 0.0;
	lastFrameTime = 0.0;

	__KernelRegisterWaitTypeFuncs(WAITTYPE_VBLANK, __DisplayVblankBeginCallback, __DisplayVblankEndCallback);
}

struct GPUStatistics_v0 {
	int firstInts[11];
	double msProcessingDisplayLists;
	int moreInts[15];
};

void __DisplayDoState(PointerWrap &p) {
	auto s = p.Section("sceDisplay", 1, 7);
	if (!s)
		return;

	Do(p, framebuf);
	Do(p, latchedFramebuf);
	Do(p, framebufIsLatched);
	DisplayHWDoState(p, s <= 2);
	Do(p, hasSetMode);
	Do(p, mode);
	Do(p, resumeMode);
	Do(p, holdMode);
	if (s >= 4) {
		Do(p, brightnessLevel);
	}
	Do(p, width);
	Do(p, height);
	WaitVBlankInfo wvi(0);
	Do(p, vblankWaitingThreads, wvi);
	Do(p, vblankPausedWaits);

	Do(p, enterVblankEvent);
	CoreTiming::RestoreRegisterEvent(enterVblankEvent, "EnterVBlank", &hleEnterVblank);
	Do(p, leaveVblankEvent);
	CoreTiming::RestoreRegisterEvent(leaveVblankEvent, "LeaveVBlank", &hleLeaveVblank);
	Do(p, afterFlipEvent);
	CoreTiming::RestoreRegisterEvent(afterFlipEvent, "AfterFlip", &hleAfterFlip);

	if (s >= 5) {
		bool lagSyncScheduled = false;
		Do(p, lagSyncScheduled);
		if (lagSyncScheduled) {
			int lagSyncEvent = -1;
			Do(p, lagSyncEvent);
		}
	}

	Do(p, gstate);

	// TODO: GPU stuff is really not the responsibility of sceDisplay.
	// Display just displays the buffers the GPU has drawn, they are really completely distinct.
	// Maybe a bit tricky to move at this point, though...

	gstate_c.DoState(p);
	if (s < 2) {
		// This shouldn't have been savestated anyway, but it was.
		// It's unlikely to overlap with the first value in gpuStats.
		int gpuVendorTemp = 0;
		p.ExpectVoid(&gpuVendorTemp, sizeof(gpuVendorTemp));
	}
	if (s < 6) {
		GPUStatistics_v0 oldStats;
		Do(p, oldStats);
	}

	if (s < 7) {
		u64 now = CoreTiming::GetTicks();
		lastFlipCycles = now;
		nextFlipCycles = now;
	} else {
		Do(p, lastFlipCycles);
		Do(p, nextFlipCycles);
	}

	gpu->DoState(p);

	if (p.mode == p.MODE_READ) {
		gpu->ReapplyGfxState();
		gpu->SetDisplayFramebuffer(framebuf.topaddr, framebuf.stride, framebuf.fmt);
	}
}

void __DisplayShutdown() {
	vblankWaitingThreads.clear();
}

void __DisplayVblankBeginCallback(SceUID threadID, SceUID prevCallbackId) {
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	// This means two callbacks in a row.  PSP crashes if the same callback waits inside itself (may need more testing.)
	// TODO: Handle this better?
	if (vblankPausedWaits.find(pauseKey) != vblankPausedWaits.end()) {
		return;
	}

	WaitVBlankInfo waitData(0);
	for (size_t i = 0; i < vblankWaitingThreads.size(); i++) {
		WaitVBlankInfo *t = &vblankWaitingThreads[i];
		if (t->threadID == threadID) {
			waitData = *t;
			vblankWaitingThreads.erase(vblankWaitingThreads.begin() + i);
			break;
		}
	}

	if (waitData.threadID != threadID) {
		WARN_LOG_REPORT(Log::sceDisplay, "sceDisplayWaitVblankCB: could not find waiting thread info.");
		return;
	}

	vblankPausedWaits[pauseKey] = __DisplayGetVCount() + waitData.vcountUnblock;
	DEBUG_LOG(Log::sceDisplay, "sceDisplayWaitVblankCB: Suspending vblank wait for callback");
}

void __DisplayVblankEndCallback(SceUID threadID, SceUID prevCallbackId) {
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	// Probably should not be possible.
	if (vblankPausedWaits.find(pauseKey) == vblankPausedWaits.end()) {
		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	int vcountUnblock = vblankPausedWaits[pauseKey];
	vblankPausedWaits.erase(pauseKey);
	if (vcountUnblock <= __DisplayGetVCount()) {
		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	// Still have to wait a bit longer.
	vblankWaitingThreads.push_back(WaitVBlankInfo(__KernelGetCurThread(), vcountUnblock - __DisplayGetVCount()));
	DEBUG_LOG(Log::sceDisplay, "sceDisplayWaitVblankCB: Resuming vblank wait from callback");
}

void __DisplaySetWasPaused() {
	wasPaused = true;
}

// All the throttling and frameskipping logic is here.
// This is called just before we drop out of the main loop, in order to allow the submit and present to happen.
static void DoFrameTiming(bool *skipFrame, float scaledTimestep, bool endOfFrame) {
	*skipFrame = false;

	// A second flip in the same vblank: no time has passed, and the pace below would take "now" for the next frame's time,
	// forgetting however far behind it is.
	if (scaledTimestep <= 0.f && lastFrameTime != 0.0 && !wasPaused)
		return;

	const bool autoFrameSkip = UseAutoFrameSkip();

	bool doFrameSkip = g_Config.iFrameSkip != 0;
	SkipMeasure(time_now_d(), scaledTimestep, wasPaused);

	if (lastFrameTime == 0.0 || wasPaused) {
		nextFrameTime = time_now_d() + scaledTimestep;
	} else {
		// Advance lastFrameTime by a constant amount each frame,
		// but don't let it get too far behind as things can get very jumpy.
		const double maxFallBehindFrames = 5.5;

		nextFrameTime = std::max(lastFrameTime + scaledTimestep, time_now_d() - maxFallBehindFrames * scaledTimestep);
	}
	curFrameTime = time_now_d();


	// Auto-frameskip automatically if speed limit is set differently than the default.
	if (autoFrameSkip) {
		// Deadstation Portable: decided each frame by how far behind the game is. Not drawn while emulation is more than 8 ms behind real
		// time (PPSSPP's test, with Deadcast's margin for the timer's jitter) and the sound queued for the speaker is under where it starts
		// (libretro's: it's about to run out), and only while skipped frames are measured cheaper than drawn ones (SkipPays). At most
		// iFrameSkip in a row (__DisplayFlip). A skipped frame saves the GE's draw work (no decode, no flush) and the GPU's.
		if (doFrameSkip && curFrameTime > nextFrameTime + 0.008 && AudioRunningLow() && SkipPays(curFrameTime)) {
			*skipFrame = true;
			statSkipped++;
			statSkipLateSum += curFrameTime - nextFrameTime;
			statSkipLateWorst = std::max(statSkipLateWorst, curFrameTime - nextFrameTime);
		}
	} else {
		const int frameSkipNum = g_Config.iFrameSkip;
		if (frameSkipNum >= 1) {
			// fixed frameskip
			if (numSkippedFrames >= frameSkipNum)
				*skipFrame = false;
			else
				*skipFrame = true;
		}
	}

	{
		// what this flip waits (the jump below: none)
		const double wait = curFrameTime < nextFrameTime && nextFrameTime - curFrameTime <= 2 * scaledTimestep ? nextFrameTime - curFrameTime : 0.0;
		GeModeMeasure(curFrameTime, wait, scaledTimestep / timePerVblank, lastFrameTime != 0.0 && !wasPaused && steadyFlips >= 60, skipLastSkipped);
	}
	// this frame will wait for real time: its time then isn't its cost (SkipMeasure)
	skipLastWaited = curFrameTime < nextFrameTime;
	if (skipLastWaited && nextFrameTime - curFrameTime <= 2 * scaledTimestep) {
		statWaited++;
		statWaitedFor += nextFrameTime - curFrameTime;
	}
	if (curFrameTime < nextFrameTime) {
		// If time gap is huge just jump (somebody fast-forwarded)
		if (nextFrameTime - curFrameTime > 2*scaledTimestep) {
			nextFrameTime = curFrameTime;
		} else {
			// Wait until we've caught up.
			// If we're ending the frame here, we'll defer the sleep until after the command buffers
			// have been handed off to the render thread, for some more overlap.
			if (endOfFrame) {
				g_frameTiming.DeferWaitUntil(nextFrameTime, &curFrameTime);
			} else {
				WaitUntil(curFrameTime, nextFrameTime, "display-wait");
				curFrameTime = time_now_d();  // I guess we could also just set it to nextFrameTime...
			}
		}
	}

	lastFrameTime = nextFrameTime;
	wasPaused = false;
}

static void DoFrameIdleTiming() {
	if (!g_Config.bEnableSound || wasPaused) {
		return;
	}

	double before = time_now_d();
	double dist = before - lastFrameTime;
	// Ignore if the distance is just crazy.  May mean wrap or pause.
	if (dist < 0.0 || dist >= 15.0 * timePerVblank) {
		return;
	}

	// If we have over at least a vblank of spare time, maintain at least 30fps in delay.
	// This prevents fast forward during loading screens.
	// Give a little extra wiggle room in case the next vblank does more work.
	const double goal = lastFrameTime + (numVBlanksSinceFlip - 1) * timePerVblank - 0.001;
	if (numVBlanksSinceFlip >= 2 && before < goal) {
		double cur_time;
		while ((cur_time = time_now_d()) < goal) {
			const double left = goal - cur_time;
			if (left > 0.0f && left < 1.0f) {  // Sanity check
				usleep((long)(left * 1000000));
			}
		}
	}
}

void hleEnterVblank(u64 userdata, int cyclesLate) {
	int vbCount = userdata;

	VERBOSE_LOG(Log::sceDisplay, "Enter VBlank %i", vbCount);

	DisplayFireVblankStart();

	CoreTiming::ScheduleEvent(msToCycles(vblankMs) - cyclesLate, leaveVblankEvent, vbCount + 1);

	// Trigger VBlank interrupt handlers.
	__TriggerInterrupt(PSP_INTR_IMMEDIATE | PSP_INTR_ONLY_IF_ENABLED | PSP_INTR_ALWAYS_RESCHED, PSP_VBLANK_INTR, PSP_INTR_SUB_ALL);

	// Wake up threads waiting for VBlank
	u32 error;
	bool wokeThreads = false;
	for (size_t i = 0; i < vblankWaitingThreads.size(); i++) {
		if (--vblankWaitingThreads[i].vcountUnblock == 0) {
			// Only wake it if it wasn't already released by someone else.
			SceUID waitID = __KernelGetWaitID(vblankWaitingThreads[i].threadID, WAITTYPE_VBLANK, error);
			if (waitID == 1) {
				__KernelResumeThreadFromWait(vblankWaitingThreads[i].threadID, 0);
				wokeThreads = true;
			}
			vblankWaitingThreads.erase(vblankWaitingThreads.begin() + i--);
		}
	}
	if (wokeThreads) {
		__KernelReSchedule("entered vblank");
	}

	numVBlanksSinceFlip++;

	// TODO: Should this be done here or in hleLeaveVblank?
	if (framebufIsLatched) {
		DEBUG_LOG(Log::sceDisplay, "Setting latched framebuffer %08x (prev: %08x)", latchedFramebuf.topaddr, framebuf.topaddr);
		framebuf = latchedFramebuf;
		framebufIsLatched = false;
		ge::display(framebuf.topaddr, framebuf.stride, framebuf.fmt);
		__DisplayFlip(cyclesLate);
	} else if (!flippedThisFrame) {
		// Gotta flip even if sceDisplaySetFramebuf was not called.
		__DisplayFlip(cyclesLate);
	}
}

static bool flipForced = false;

bool __DisplayTakeFlipForced() {
	const bool forced = flipForced;
	flipForced = false;
	return forced;
}

void __DisplayFlip(int cyclesLate) {
	flippedThisFrame = true;
	// We flip only if the framebuffer was dirty. This eliminates flicker when using
	// non-buffered rendering. The interaction with frame skipping seems to need
	// some work.
	// But, let's flip at least once every 10 vblanks, to update fps, etc.
	const bool noRecentFlip = !g_Config.bSkipBufferEffects && numVBlanksSinceFlip >= 10;

	// Whether there really is a new picture is for the GE to say, when it has got to this point (the host frame's end). Here:
	// could there be.
	const bool fbDirty = ge::takeWorked();

	if (!fbDirty && !noRecentFlip) {
		// Okay, there's no new frame to draw, game might be sitting in a static loading screen
		// or similar, and not long enough to trigger noRecentFlip. But audio may be playing, so we need to time still.
		DoFrameIdleTiming();
		return;
	}

	DisplayFireFlip();

	// Setting CORE_NEXTFRAME (which Core_NextFrame does) causes a swap.
	bool nextFrame = Core_NextFrame();
	if (!nextFrame) {
		WARN_LOG(Log::sceDisplay, "Core_NextFrame returned false");
	}
	flipForced = noRecentFlip;

	if (fbDirty) {
		gpuStats.numFlips++;
	}

	bool skipFrame;
	DoFrameTiming(&skipFrame, (float)numVBlanksSinceFlip * timePerVblank, nextFrame);

	// 4 here means 1 drawn, 4 skipped - so 12 fps minimum.
	if (numSkippedFrames >= g_Config.iFrameSkip) {
		skipFrame = false;
	}

	// Tell the emulated GPU whether to skip the next frame.
	ge::skipFrame(skipFrame);
	skipLastSkipped = skipFrame;
	if (skipFrame) {
		numSkippedFrames++;
	} else {
		numSkippedFrames = 0;
	}

	// Returning here with coreState == CORE_NEXTFRAME causes a buffer flip to happen (next frame).
	// Right after, we regain control for a little bit in hleAfterFlip. I think that's a great
	// place to do housekeeping.

	CoreTiming::ScheduleEvent(0 - cyclesLate, afterFlipEvent, 0);
	numVBlanksSinceFlip = 0;
}

void hleAfterFlip(u64 userdata, int cyclesLate) {
	ge::pspFrame();

	PPGeNotifyFrame();

}

void hleLeaveVblank(u64 userdata, int cyclesLate) {
	flippedThisFrame = false;
	VERBOSE_LOG(Log::sceDisplay,"Leave VBlank %i", (int)userdata - 1);
	CoreTiming::ScheduleEvent(msToCycles(frameMs - vblankMs) - cyclesLate, enterVblankEvent, userdata);

	// Fire the vblank listeners after the vblank completes.
	DisplayFireVblankEnd();
}


static u32 sceDisplayIsVblank() {
	return hleLogDebug(Log::sceDisplay, DisplayIsVblank());
}

void __DisplayWaitForVblanks(const char *reason, int vblanks, bool callbacks) {
	const s64 ticksIntoFrame = CoreTiming::GetTicks() - DisplayFrameStartTicks();
	const s64 cyclesToNextVblank = msToCycles(frameMs) - ticksIntoFrame;

	// These syscalls take about 115 us, so if the next vblank is before then, we're waiting extra.
	// At least, on real firmware a wait >= 16500 into the frame will wait two.
	if (cyclesToNextVblank <= usToCycles(115)) {
		++vblanks;
	}

	vblankWaitingThreads.push_back(WaitVBlankInfo(__KernelGetCurThread(), vblanks));
	__KernelWaitCurThread(WAITTYPE_VBLANK, 1, 0, 0, callbacks, reason);
}

static u32 sceDisplaySetMode(int displayMode, int displayWidth, int displayHeight) {
	if (displayMode != PSP_DISPLAY_MODE_LCD || displayWidth != 480 || displayHeight != 272) {
		WARN_LOG_REPORT(Log::sceDisplay, "Video out requested, not supported: mode=%d size=%d,%d", displayMode, displayWidth, displayHeight);
	}
	if (displayMode != PSP_DISPLAY_MODE_LCD) {
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_MODE, "invalid mode");
	}
	if (displayWidth != 480 || displayHeight != 272) {
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_SIZE, "invalid size");
	}

	hasSetMode = true;
	mode = displayMode;
	width = displayWidth;
	height = displayHeight;

	// On success, this implicitly waits for a vblank start.
	__DisplayWaitForVblanks("display mode", 1);
	return hleLogDebug(Log::sceDisplay, 0);
}

void __DisplaySetFramebuf(u32 topaddr, int linesize, int pixelFormat, int sync) {
	FrameBufferState fbstate = {0};
	fbstate.topaddr = topaddr;
	fbstate.fmt = (GEBufferFormat)pixelFormat;
	fbstate.stride = linesize;

	if (sync == PSP_DISPLAY_SETBUF_IMMEDIATE) {
		// Write immediately to the current framebuffer parameters.
		framebuf = fbstate;
		// Also update latchedFramebuf for any sceDisplayGetFramebuf() after this.
		latchedFramebuf = fbstate;
		ge::display(framebuf.topaddr, framebuf.stride, framebuf.fmt);
		// IMMEDIATE means that the buffer is fine. We can just flip immediately.
		// Doing it in non-buffered though creates problems (black screen) on occasion though
		// so let's not.
		if (!flippedThisFrame && !g_Config.bSkipBufferEffects) {
			double before_flip = time_now_d();
			__DisplayFlip(0);
			double after_flip = time_now_d();
			// Ignore for debug stats.
			hleSetFlipTime(after_flip - before_flip);
		}
	} else {
		// Delay the write until vblank
		latchedFramebuf = fbstate;
		framebufIsLatched = true;

		// If we update the format or stride, this affects the current framebuf immediately.
		framebuf.fmt = latchedFramebuf.fmt;
		framebuf.stride = latchedFramebuf.stride;
	}
}

// Some games (GTA) never call this during gameplay, so bad place to put a framerate counter.
int sceDisplaySetFramebuf(u32 topaddr, int linesize, int pixelformat, int sync) {
	if (sync != PSP_DISPLAY_SETBUF_IMMEDIATE && sync != PSP_DISPLAY_SETBUF_NEXTFRAME) {
		return hleLogError(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_MODE, "invalid sync mode");
	}
	if (topaddr != 0 && !Memory::IsRAMAddress(topaddr) && !Memory::IsVRAMAddress(topaddr)) {
		return hleLogError(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_POINTER, "invalid address");
	}
	if ((topaddr & 0xF) != 0) {
		return hleLogError(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_POINTER, "misaligned address");
	}
	if ((linesize & 0x3F) != 0 || (linesize == 0 && topaddr != 0)) {
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_SIZE, "invalid stride");
	}
	if (pixelformat < 0 || pixelformat > GE_FORMAT_8888) {
		return hleLogError(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_FORMAT, "invalid format");
	}

	if (sync == PSP_DISPLAY_SETBUF_IMMEDIATE) {
		if ((GEBufferFormat)pixelformat != latchedFramebuf.fmt || linesize != latchedFramebuf.stride) {
			return hleReportWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_MODE, "must change latched framebuf first");
		}
	}

	hleEatCycles(290);

	s64 delayCycles = 0;
	// Don't count transitions between display off and display on.
	if (topaddr != 0 &&
		(topaddr != framebuf.topaddr || PSP_CoreParameter().compat.flags().SplitFramebufferMargin) &&
		framebuf.topaddr != 0 &&
		PSP_CoreParameter().compat.flags().ForceMax60FPS) {
		// sceDisplaySetFramebuf() isn't supposed to delay threads at all.  This is a hack.
		// So let's only delay when it's more than 1ms.
		const s64 FLIP_DELAY_CYCLES_MIN = usToCycles(1000);
		// Some games (like Final Fantasy 4) only call this too much in spurts.
		// The goal is to fix games where this would result in a consistent overhead.
		const int FLIP_DELAY_MIN_FLIPS = 30;
		// Since we move nextFlipCycles forward a whole frame each time, we allow it to be a little ahead.
		// Otherwise it'll always be ahead if the game messes up even once.
		const s64 LEEWAY_CYCLES_PER_FLIP = usToCycles(10);

		u64 now = CoreTiming::GetTicks();
		s64 cyclesAhead = nextFlipCycles - now;
		if (cyclesAhead > FLIP_DELAY_CYCLES_MIN) {
			if (lastFlipsTooFrequent >= FLIP_DELAY_MIN_FLIPS) {
				delayCycles = cyclesAhead;
			} else {
				++lastFlipsTooFrequent;
			}
		} else if (-lastFlipsTooFrequent < FLIP_DELAY_MIN_FLIPS) {
			--lastFlipsTooFrequent;
		}

		// 1001 to account for NTSC timing (59.94 fps.)
		u64 expected = msToCycles(1001) / framerate - LEEWAY_CYCLES_PER_FLIP;
		lastFlipCycles = now;
		nextFlipCycles = std::max(lastFlipCycles, nextFlipCycles) + expected;
	}

	__DisplaySetFramebuf(topaddr, linesize, pixelformat, sync);

	// No delaying while inside an interrupt.  It'll cause idle threads to starve.
	if (delayCycles > 0 && !__IsInInterrupt()) {
		// Okay, the game is going at too high a frame rate.  God of War and Fat Princess both do this.
		// Simply eating the cycles works and is fast, but breaks other games (like Jeanne d'Arc.)
		// So, instead, we delay this HLE thread only (a small deviation from correct behavior.)
		return hleDelayResult(hleLogDebug(Log::sceDisplay, 0, "delaying frame thread"), "set framebuf", cyclesToUs(delayCycles));
	} else {
		if (topaddr == 0) {
			return hleLogDebug(Log::sceDisplay, 0, "disabling display");
		} else {
			return hleLogDebug(Log::sceDisplay, 0);
		}
	}
}

bool __DisplayGetFramebuf(PSPPointer<u8> *topaddr, u32 *linesize, u32 *pixelFormat, int latchedMode) {
	const FrameBufferState &fbState = latchedMode == PSP_DISPLAY_SETBUF_NEXTFRAME ? latchedFramebuf : framebuf;
	if (topaddr != nullptr)
		(*topaddr).ptr = fbState.topaddr;
	if (linesize != nullptr)
		*linesize = fbState.stride;
	if (pixelFormat != nullptr)
		*pixelFormat = fbState.fmt;

	return true;
}

static u32 sceDisplayGetFramebuf(u32 topaddrPtr, u32 linesizePtr, u32 pixelFormatPtr, int latchedMode) {
	const FrameBufferState &fbState = latchedMode == PSP_DISPLAY_SETBUF_NEXTFRAME ? latchedFramebuf : framebuf;

	if (Memory::IsValidAddress(topaddrPtr))
		Memory::Write_U32(fbState.topaddr, topaddrPtr);
	if (Memory::IsValidAddress(linesizePtr))
		Memory::Write_U32(fbState.stride, linesizePtr);
	if (Memory::IsValidAddress(pixelFormatPtr))
		Memory::Write_U32(fbState.fmt, pixelFormatPtr);

	return hleLogDebug(Log::sceDisplay, 0);
}

static void __DisplayWaitForVblanksCB(const char *reason, int vblanks) {
	__DisplayWaitForVblanks(reason, vblanks, true);
}

static int sceDisplayWaitVblankStart() {
	__DisplayWaitForVblanks("vblank start waited", 1);
	return hleLogDebug(Log::sceDisplay, 0);
}

static int sceDisplayWaitVblank() {
	if (!DisplayIsVblank()) {
		__DisplayWaitForVblanks("vblank waited", 1);
		return hleLogDebug(Log::sceDisplay, 0);
	} else {
		hleEatCycles(1110);
		hleReSchedule("vblank wait skipped");
		return hleLogDebug(Log::sceDisplay, 1, "not waiting since in vblank");
	}
}

static int sceDisplayWaitVblankStartMulti(int vblanks) {
	if (vblanks <= 0) {
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid number of vblanks");
	}
	if (!__KernelIsDispatchEnabled())
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	if (__IsInInterrupt())
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "in interrupt");

	__DisplayWaitForVblanks("vblank start multi waited", vblanks);
	return hleLogDebug(Log::sceDisplay, 0);
}

static int sceDisplayWaitVblankCB() {
	if (!DisplayIsVblank()) {
		__DisplayWaitForVblanksCB("vblank waited", 1);
		return hleLogDebug(Log::sceDisplay, 0);
	} else {
		hleEatCycles(1110);
		hleReSchedule("vblank wait skipped");
		return hleLogDebug(Log::sceDisplay, 1, "not waiting since in vblank");
	}
}

static int sceDisplayWaitVblankStartCB() {
	__DisplayWaitForVblanksCB("vblank start waited", 1);
	return hleLogDebug(Log::sceDisplay, 0);
}

static int sceDisplayWaitVblankStartMultiCB(int vblanks) {
	if (vblanks <= 0) {
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid number of vblanks");
	}
	if (!__KernelIsDispatchEnabled())
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	if (__IsInInterrupt())
		return hleLogWarning(Log::sceDisplay, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "in interrupt");

	__DisplayWaitForVblanksCB("vblank start multi waited", vblanks);
	return hleLogDebug(Log::sceDisplay, 0);
}

static int sceDisplayGetVcount() {
	hleEatCycles(150);
	hleReSchedule("get vcount");
	return hleLogVerbose(Log::sceDisplay, __DisplayGetVCount());
}

static int sceDisplayGetCurrentHcount() {
	hleEatCycles(275);
	return hleLogDebug(Log::sceDisplay, __DisplayGetCurrentHcount());
}

static int sceDisplayAdjustAccumulatedHcount(int value) {
	if (value < 0) {
		return hleLogError(Log::sceDisplay, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid value");
	}

	// Since it includes the current hCount, find the difference to apply to the base.
	u32 accumHCount = __DisplayGetAccumulatedHcount();
	int diff = value - accumHCount;
	DisplayAdjustAccumulatedHcount(diff);

	return hleLogDebug(Log::sceDisplay, 0);
}

static int sceDisplayGetAccumulatedHcount() {
	u32 accumHCount = __DisplayGetAccumulatedHcount();
	hleEatCycles(235);
	return hleLogDebug(Log::sceDisplay, accumHCount);
}

static float sceDisplayGetFramePerSec() {
	const static float framePerSec = 59.9400599f;
	return hleLogVerbose(Log::sceDisplay, framePerSec);	// (9MHz * 1)/(525 * 286)
}

static u32 sceDisplayIsForeground() {
	int result = hasSetMode && framebuf.topaddr != 0 ? 1 : 0;
	return hleLogDebug(Log::sceDisplay, result);
}

static u32 sceDisplayGetMode(u32 modeAddr, u32 widthAddr, u32 heightAddr) {
	if (Memory::IsValidAddress(modeAddr))
		Memory::Write_U32(mode, modeAddr);
	if (Memory::IsValidAddress(widthAddr))
		Memory::Write_U32(width, widthAddr);
	if (Memory::IsValidAddress(heightAddr))
		Memory::Write_U32(height, heightAddr);
	return hleLogDebug(Log::sceDisplay, 0);
}

static u32 sceDisplayIsVsync() {
	u64 now = CoreTiming::GetTicks();
	u64 start = DisplayFrameStartTicks() + msToCycles(vsyncStartMs);
	u64 end = DisplayFrameStartTicks() + msToCycles(vsyncEndMs);

	return hleLogDebug(Log::sceDisplay, now >= start && now <= end ? 1 : 0);
}

static u32 sceDisplayGetResumeMode(u32 resumeModeAddr) {
	if (Memory::IsValidAddress(resumeModeAddr))
		Memory::Write_U32(resumeMode, resumeModeAddr);
	return hleLogDebug(Log::sceDisplay, 0);
}

static u32 sceDisplaySetResumeMode(u32 rMode) {
	// Not sure what this does, seems to do nothing in tests and accept all values.
	resumeMode = rMode;
	return hleReportError(Log::sceDisplay, 0, "unsupported");
}

static u32 sceDisplayGetBrightness(u32 levelAddr, u32 otherAddr) {
	// Standard levels on a PSP: 44, 60, 72, 84 (AC only)

	if (Memory::IsValidAddress(levelAddr)) {
		Memory::Write_U32(brightnessLevel, levelAddr);
	}
	// Always seems to write zero?
	if (Memory::IsValidAddress(otherAddr)) {
		Memory::Write_U32(0, otherAddr);
	}
	return hleLogWarning(Log::sceDisplay, 0);
}

static u32 sceDisplaySetBrightness(int level, int other) {
	// Note: Only usable in kernel mode.
	brightnessLevel = level;
	return hleLogWarning(Log::sceDisplay, 0);
}

static u32 sceDisplaySetHoldMode(u32 hMode) {
	// Not sure what this does, seems to do nothing in tests and accept all values.
	holdMode = hMode;
	return hleLogWarning(Log::sceDisplay, 0, "UNIMPL");
}

const HLEFunction sceDisplay[] = {
	{0X0E20F177, &WrapU_III<sceDisplaySetMode>,               "sceDisplaySetMode",                 'x', "iii" },
	{0X289D82FE, &WrapI_UIII<sceDisplaySetFramebuf>,          "sceDisplaySetFrameBuf",             'i', "xiii"},
	{0XEEDA2E54, &WrapU_UUUI<sceDisplayGetFramebuf>,          "sceDisplayGetFrameBuf",             'x', "pppi"},
	{0X36CDFADE, &WrapI_V<sceDisplayWaitVblank>,              "sceDisplayWaitVblank",              'i', "",   HLE_NOT_DISPATCH_SUSPENDED },
	{0X984C27E7, &WrapI_V<sceDisplayWaitVblankStart>,         "sceDisplayWaitVblankStart",         'i', "",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X40F1469C, &WrapI_I<sceDisplayWaitVblankStartMulti>,    "sceDisplayWaitVblankStartMulti",    'i', "i"   },
	{0X8EB9EC49, &WrapI_V<sceDisplayWaitVblankCB>,            "sceDisplayWaitVblankCB",            'i', "",   HLE_NOT_DISPATCH_SUSPENDED },
	{0X46F186C3, &WrapI_V<sceDisplayWaitVblankStartCB>,       "sceDisplayWaitVblankStartCB",       'i', "",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X77ED8B3A, &WrapI_I<sceDisplayWaitVblankStartMultiCB>,  "sceDisplayWaitVblankStartMultiCB",  'i', "i"   },
	{0XDBA6C4C4, &WrapF_V<sceDisplayGetFramePerSec>,          "sceDisplayGetFramePerSec",          'f', ""    },
	{0X773DD3A3, &WrapI_V<sceDisplayGetCurrentHcount>,        "sceDisplayGetCurrentHcount",        'i', ""    },
	{0X210EAB3A, &WrapI_V<sceDisplayGetAccumulatedHcount>,    "sceDisplayGetAccumulatedHcount",    'i', ""    },
	{0XA83EF139, &WrapI_I<sceDisplayAdjustAccumulatedHcount>, "sceDisplayAdjustAccumulatedHcount", 'i', "i"   },
	{0X9C6EAAD7, &WrapI_V<sceDisplayGetVcount>,               "sceDisplayGetVcount",               'i', ""    },
	{0XDEA197D4, &WrapU_UUU<sceDisplayGetMode>,               "sceDisplayGetMode",                 'x', "ppp" },
	{0X7ED59BC4, &WrapU_U<sceDisplaySetHoldMode>,             "sceDisplaySetHoldMode",             'x', "x"   },
	{0XA544C486, &WrapU_U<sceDisplaySetResumeMode>,           "sceDisplaySetResumeMode",           'x', "x"   },
	{0XBF79F646, &WrapU_U<sceDisplayGetResumeMode>,           "sceDisplayGetResumeMode",           'x', "p"   },
	{0XB4F378FA, &WrapU_V<sceDisplayIsForeground>,            "sceDisplayIsForeground",            'x', ""    },
	{0X31C4BAA8, &WrapU_UU<sceDisplayGetBrightness>,          "sceDisplayGetBrightness",           'x', "pp"  },
	{0X9E3C6DC6, &WrapU_II<sceDisplaySetBrightness>,          "sceDisplaySetBrightness",           'x', "ii"  },
	{0X4D4E10EC, &WrapU_V<sceDisplayIsVblank>,                "sceDisplayIsVblank",                'x', ""    },
	{0X21038913, &WrapU_V<sceDisplayIsVsync>,                 "sceDisplayIsVsync",                 'x', ""    },
};

void Register_sceDisplay() {
	RegisterHLEModule("sceDisplay", ARRAY_SIZE(sceDisplay), sceDisplay);
}

void Register_sceDisplay_driver() {
	RegisterHLEModule("sceDisplay_driver", ARRAY_SIZE(sceDisplay), sceDisplay);
}

static void __DisplaySetFramerate(void) {
	framerate = 60;
	timePerVblank = 1.0 / panelRefreshHz;
	frameMs = 1000.0 / panelRefreshHz;
}

void __DisplaySetPanelRate(double hz) {
	panelRefreshHz = hz;
	__DisplaySetFramerate();
}
