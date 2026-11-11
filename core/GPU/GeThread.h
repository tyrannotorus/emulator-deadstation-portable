// The GE on a core of its own. The game's CPU thread and the GE overlap, as they do on a PSP: display lists run here, in order
// with everything else posted, and this thread owns the host frame. The CPU thread waits only where the game can see the GE:
// the sceGe calls that read its state, a list with a handler for its interrupts, and memory operations the GE has to look at.
// Nothing here changes what the game's clock says: a list's interrupts and sync events carry the time they would have had,
// and reach the CPU thread as soon as the list has run. Late, if the GE is behind; never early.
#pragma once
#include <string>
#include "Common/CommonTypes.h"
#include "GPU/GPUDefinitions.h"
#include "GPU/ge_constants.h"

#include <atomic>
#include <functional>
#include <type_traits>
#include <utility>

namespace ge
{

struct HostFrame
{
	std::function<void()> begin;
	// the frame's end, if there is a new picture or force says so: false, with the frame left open, if not
	std::function<bool(bool force)> end;
};

// the thread, from here until stop (the emulator's main)
void start(HostFrame frame);
void stop();

// CPU thread: posted, in order. The game goes on
bool active();
void runLists(u64 ticks);
// The same when the CPU would wait for all of them anyway (a list with a handler): run here, on the CPU thread, once everything
// posted before them is done. No handoff either way, and the frame's serial work is one thread's, where the clock's governor
// sees it: two threads taking turns looked 34-41% busy on every core, and Auto held 1.4 GHz on average with Tekken 6 at 72%.
// Their interrupts and sync events reach the game as they're raised, with the times they'd have had (postInterrupt).
void runListsHere(u64 ticks);
// a list's stall moved, the lists then run from it at ticks (the CPU's time, as each run's interrupts are timed from it)
void updateStall(int listid, u32 stall, u64 ticks);
// has anything been posted whose events the game hasn't been given yet
bool unreleased();
void display(u32 framebuf, u32 stride, GEBufferFormat format);
void pspFrame();
// The game flipped to a new picture: the GPU's count of them (gpuStats.numFlips) goes up in order with what's posted, so a list
// sees the count it had when it was posted, whatever the GE thread's lag (texture rehashing, framebuffer ageing and effect
// skipping go by it).
void flipped();
void skipFrame(bool skip);
void invalidate(u32 addr, int size, GPUInvalidationType type);
// A guest frame has ended: the game goes on once the GE has run its lists (the host frame's end, the flip's wait in it, can still
// overlap the next frame). A GE a frame behind would read what the game has since rewritten for its next one.
void endFrame(bool force);
// has anything been posted that could change the picture, since the last call
bool takeWorked();

// CPU thread: everything posted has been done, and what it has for the game has been delivered
void sync();
// The same when only the display lists matter: the end of a host frame doesn't touch them, and can go on while the CPU does.
void syncLists();
// the same with the host frame closed: for the emulator's main, before it does something between frames
void pause();
bool busy();

// runs on the GE's thread while the CPU thread waits
void call(const std::function<void()>& what);
template <class F>
auto invoke(F what) -> decltype(what())
{
	if constexpr (std::is_void_v<decltype(what())>)
		call(what);
	else
	{
		decltype(what()) result {};
		call([&] { result = what(); });
		return result;
	}
}

// GE thread: for the CPU thread
void postInterrupt(int listid, u32 pc, u64 atTicks);
void postSync(GPUSyncType type, int id, u64 atTicks);
// GE interrupts when the GE's thread has got to them: lists the game has handlers for run there too, the CPU going on, and what
// they raise reaches the game at the next point of its clock that waits for the GE (a sync, the next frame's end, idle with such
// a list open: idleSync), late by a set amount of the game's time. Off: the CPU waits for such lists (runListsHere) and the stall
// updates that end them.
extern bool lateEvents;
// how it's chosen: -1 by measurement (sceDisplay.cpp, GeModeMeasure), 0 never, 1 always (tests: DEADSTATION_PORTABLE_GELATE)
extern int lateMode;
// __GeIdleSync: the lists on the GE's thread run out, and what they raised is scheduled; true if anything was
bool idleSync();
void deliverEvents();

// (tests) nanoseconds so far: the GE's thread at work, the CPU thread waiting for it. Measured only once asked for (measure,
// before start): each measurement is a clock read.
void measure(bool on);
u64 busyNs();
u64 cpuNs();
u64 waitedNs();
// (perf) where the CPU thread waited for the GE's since the last call: "offset ms/times", the most first
std::string takeWaitSites();

}

#define GE_CALL(expr) ge::invoke([&]() { return expr; })
