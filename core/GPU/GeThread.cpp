#include <sys/resource.h>
#include "GPU/GeThread.h"

#include "Core/HLE/sceGe.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "deadstation/profiler.h"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <string>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#include <time.h>

namespace ge
{
namespace
{

struct Command
{
	enum Kind { Lists, Display, PspFrame, Flip, SkipFrame, Invalidate, FrameEnd, Close, Call, Stall } kind;
	u32 a = 0, b = 0;
	int c = 0;
	u64 ticks = 0;
	const std::function<void()> *what = nullptr;
	u64 seq = 0;
};

struct Event
{
	bool interrupt;
	int id;
	u32 pc;
	u64 ticks;
	GPUSyncType type;
	u64 seq;	// the command that raised it
};

std::thread thread;
std::atomic<bool> running { false };
std::thread::id threadId;
HostFrame host;
bool frameOpen = false;		// GE's thread

std::mutex mutex;
std::condition_variable wake, done;
std::deque<Command> queue;
bool working = false;
bool runningFrameEnd = false;
bool stopping = false;
int framesAhead = 0;

std::mutex eventsMutex;
std::vector<Event> events;

// What the GE raises reaches the game only at fixed points of the game's own clock, never whenever the host's GE thread gets there:
// each command is numbered as it's posted, and the CPU thread hands over the events of commands up to one it has waited for. The
// points: the sceGe calls that wait for the GE (and enqueues, and lists run here), idle with a handler's list open (late events),
// and each frame's end, for everything up to the frame before (endFrame's wait).
u64 postedSeq = 0;		// CPU thread, under mutex
u64 runningSeq = 0;		// GE's thread
u64 releasedSeq = 0;	// CPU thread
u64 lastFrameEndSeq = 0, prevFrameEndSeq = 0;	// CPU thread

// CPU thread: the game is given what the GE raised up to command upTo, which has run
void release(u64 upTo)
{
	if (upTo > releasedSeq)
		releasedSeq = upTo;
	deliverEvents();
}

std::atomic<bool> worked { true };
// GE's thread: running a stall update the CPU didn't wait for (sceGeListUpdateStallAddr found nothing in it the game could see)
bool quietRun = false;

// such a run raised an event after all: the CPU went on, so it reaches the game late. Never expected; said out loud.
void quietRunRaised(const char *what, int id)
{
	if (lateEvents)
		return;
	static int told = 0;
	if (told < 20)
		std::fprintf(stderr, "Deadstation Portable: A STALL UPDATE RUN WITHOUT A WAIT RAISED A GE %s (list %d): it reaches the game late\n", what, id);
	told++;
}
std::atomic<u64> busyTotal { 0 }, waitedTotal { 0 }, cpuTotal { 0 };

// (perf) the CPU thread's waits for the GE's, by the code that waited: offsets in the binary, for addr2line on the unstripped one
struct WaitSite { uintptr_t at = 0; u64 ns = 0; u32 count = 0; };
WaitSite waitSites[16];
void noteWait(const void *at, u64 ns)
{
	for (WaitSite &site : waitSites)
	{
		if (site.at == (uintptr_t)at || site.at == 0)
		{
			site.at = (uintptr_t)at;
			site.ns += ns;
			site.count++;
			return;
		}
	}
}

u64 nowNs()
{
	return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void openFrame()
{
	if (!frameOpen)
	{
		host.begin();
		frameOpen = true;
	}
}

void run(const Command& c)
{
	switch (c.kind)
	{
	case Command::Lists:
		openFrame();
		gpu->ProcessDLQueue(c.ticks);
		break;
	case Command::Display:
		openFrame();
		gpu->SetDisplayFramebuffer(c.a, c.b, (GEBufferFormat)c.c);
		break;
	case Command::PspFrame:
		gpu->PSPFrame();
		break;
	case Command::Flip:
		gpuStats.numFlips++;
		break;
	case Command::SkipFrame:
		if (c.a != 0)
			gstate_c.skipDrawReason |= SKIPDRAW_SKIPFRAME;
		else
			gstate_c.skipDrawReason &= ~SKIPDRAW_SKIPFRAME;
		break;
	case Command::Invalidate:
		openFrame();
		gpu->InvalidateCache(c.a, c.c, (GPUInvalidationType)c.b);
		break;
	case Command::FrameEnd:
		openFrame();
		if (host.end(c.a != 0))
			frameOpen = false;
		break;
	case Command::Close:
		if (frameOpen && host.end(true))
			frameOpen = false;
		break;
	case Command::Call:
		openFrame();
		(*c.what)();
		break;
	case Command::Stall:
	{
		// a list's new stall, then the lists run from it, as sceGeListUpdateStallAddr and __GeRunLists would have
		openFrame();
		bool runList = false;
		gpu->UpdateStall(c.c, c.a, &runList);
		quietRun = true;
		if (runList)
			gpu->ProcessDLQueue(c.ticks);
		quietRun = false;
		break;
	}
	}
}

void loop()
{
	pthread_setname_np(pthread_self(), "dsp-ge");
	// Ahead of the kernel's and everyone else's small jobs: a fight's frame has no time to give them
	setpriority(PRIO_PROCESS, 0, -5);
	deadstation::profiler::startThread("ge");
	std::unique_lock<std::mutex> lock(mutex);
	while (true)
	{
		wake.wait(lock, [] { return stopping || !queue.empty(); });
		if (queue.empty())
			break;
		const Command command = queue.front();
		queue.pop_front();
		working = true;
		runningSeq = command.seq;
		runningFrameEnd = command.kind == Command::FrameEnd;
		if (runningFrameEnd)
			done.notify_all();
		lock.unlock();
		const u64 from = nowNs();
		run(command);
		busyTotal += nowNs() - from;
		timespec cpu;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
		cpuTotal = (u64)cpu.tv_sec * 1000000000 + cpu.tv_nsec;
		lock.lock();
		working = false;
		if (command.kind == Command::FrameEnd)
			framesAhead--;
		done.notify_all();
	}
}

void post(Command command)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		command.seq = ++postedSeq;
		queue.push_back(command);
		if (command.kind == Command::FrameEnd)
			framesAhead++;
	}
	wake.notify_one();
}

bool onThread()
{
	return std::this_thread::get_id() == threadId;
}

}	// namespace

std::atomic<bool> eventsWaiting { false };
bool lateEvents = false;
int lateMode = -1;

void start(HostFrame frame)
{
	host = std::move(frame);
	frameOpen = false;
	stopping = false;
	running = true;
	thread = std::thread(loop);
	threadId = thread.get_id();
}

void stop()
{
	if (!running)
		return;
	pause();
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	wake.notify_one();
	thread.join();
	running = false;
	threadId = std::thread::id();
}

void runLists(u64 ticks)
{
	worked = true;
	if (!running)
	{
		gpu->ProcessDLQueue(ticks);
		return;
	}
	Command c { Command::Lists };
	c.ticks = ticks;
	post(c);
}

void runListsHere(u64 ticks)
{
	const void *caller = __builtin_return_address(0);
	worked = true;
	if (!running || onThread())
	{
		gpu->ProcessDLQueue(ticks);
		return;
	}
	{
		const u64 from = nowNs();
		bool waited = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (working || !queue.empty())
				waited = true;
			done.wait(lock, [] { return !working && queue.empty(); });
		}
		if (waited)
			{ const u64 waitedFor = nowNs() - from; waitedTotal += waitedFor; noteWait(caller, waitedFor); }
	}
	// what the GE raised before these lists reaches the game ahead of what they raise
	release(postedSeq);
	// the GE's thread is idle, and only this thread posts to it: the host frame and the GPU are this thread's until it does
	const u64 from = nowNs();
	openFrame();
	gpu->ProcessDLQueue(ticks);
	busyTotal += nowNs() - from;
}

bool active()
{
	return running;
}

void updateStall(int listid, u32 stall, u64 ticks)
{
	worked = true;
	Command c { Command::Stall };
	c.a = stall;
	c.c = listid;
	c.ticks = ticks;
	post(c);
}

void deliver()
{
	if (eventsWaiting.load(std::memory_order_relaxed))
		deliverEvents();
}

bool unreleased()
{
	return running && postedSeq > releasedSeq;
}

void display(u32 framebuf, u32 stride, GEBufferFormat format)
{
	worked = true;
	if (!running)
	{
		gpu->SetDisplayFramebuffer(framebuf, stride, format);
		return;
	}
	Command c { Command::Display };
	c.a = framebuf;
	c.b = stride;
	c.c = (int)format;
	post(c);
}

void pspFrame()
{
	if (!running)
	{
		gpu->PSPFrame();
		return;
	}
	post(Command { Command::PspFrame });
}

void flipped()
{
	if (!running)
	{
		gpuStats.numFlips++;
		return;
	}
	post(Command { Command::Flip });
}

void skipFrame(bool skip)
{
	Command c { Command::SkipFrame };
	c.a = skip ? 1 : 0;
	if (!running)
	{
		run(c);
		return;
	}
	post(c);
}

void invalidate(u32 addr, int size, GPUInvalidationType type)
{
	if (!running)
	{
		gpu->InvalidateCache(addr, size, type);
		return;
	}
	Command c { Command::Invalidate };
	c.a = addr;
	c.b = (u32)type;
	c.c = size;
	post(c);
}

void endFrame(bool force)
{
	const void *caller = __builtin_return_address(0);
	Command c { Command::FrameEnd };
	c.a = force ? 1 : 0;
	post(c);
	prevFrameEndSeq = lastFrameEndSeq;
	lastFrameEndSeq = postedSeq;
	const u64 from = nowNs();
	bool waited = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (framesAhead > 1)
			waited = true;
		done.wait(lock, [] { return framesAhead <= 1; });
	}
	if (waited)
		{ const u64 waitedFor = nowNs() - from; waitedTotal += waitedFor; noteWait(caller, waitedFor); }
	// at most this frame's end is still ahead: everything up to the last one has run
	release(prevFrameEndSeq);
}

bool takeWorked()
{
	return worked.exchange(false);
}

void sync()
{
	const void *caller = __builtin_return_address(0);
	if (running && !onThread())
	{
		const u64 from = nowNs();
		bool waited = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (working || !queue.empty())
				waited = true;
			done.wait(lock, [] { return !working && queue.empty(); });
		}
		if (waited)
			{ const u64 waitedFor = nowNs() - from; waitedTotal += waitedFor; noteWait(caller, waitedFor); }
	}
	release(postedSeq);
}

void syncLists()
{
	const void *caller = __builtin_return_address(0);
	if (running && !onThread())
	{
		const u64 from = nowNs();
		bool waited = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			if ((working && !runningFrameEnd) || !queue.empty())
				waited = true;
			done.wait(lock, [] { return queue.empty() && (!working || runningFrameEnd); });
		}
		if (waited)
			{ const u64 waitedFor = nowNs() - from; waitedTotal += waitedFor; noteWait(caller, waitedFor); }
	}
	release(postedSeq);
}

bool idleSync()
{
	if (!running || onThread())
		return false;
	const void *caller = __builtin_return_address(0);
	const u64 from = nowNs();
	{
		std::unique_lock<std::mutex> lock(mutex);
		done.wait(lock, [] { return queue.empty() && (!working || runningFrameEnd); });
	}
	{ const u64 waitedFor = nowNs() - from; waitedTotal += waitedFor; noteWait(caller, waitedFor); }
	const bool any = eventsWaiting.load(std::memory_order_relaxed);
	release(postedSeq);
	return any;
}

void pause()
{
	if (running)
		post(Command { Command::Close });
	sync();
}

bool busy()
{
	if (!running)
		return false;
	std::lock_guard<std::mutex> lock(mutex);
	return working || !queue.empty();
}

void call(const std::function<void()>& what)
{
	if (!running || onThread())
	{
		what();
		return;
	}
	Command c { Command::Call };
	c.what = &what;
	post(c);
	sync();
}

void postInterrupt(int listid, u32 pc, u64 atTicks)
{
	if (!running || !onThread())
	{
		__GeTriggerInterrupt(listid, pc, atTicks);
		return;
	}
	if (quietRun)
		quietRunRaised("INTERRUPT", listid);
	std::lock_guard<std::mutex> lock(eventsMutex);
	events.push_back({ true, listid, pc, atTicks, GPU_SYNC_DRAW, runningSeq });
	eventsWaiting = true;
}

void postSync(GPUSyncType type, int id, u64 atTicks)
{
	if (!running || !onThread())
	{
		__GeTriggerSync(type, id, atTicks);
		return;
	}
	if (quietRun)
		quietRunRaised("SYNC", id);
	std::lock_guard<std::mutex> lock(eventsMutex);
	events.push_back({ false, id, 0, atTicks, type, runningSeq });
	eventsWaiting = true;
}

void deliverEvents()
{
	std::vector<Event> now;
	{
		std::lock_guard<std::mutex> lock(eventsMutex);
		// in the order raised, so the commands' numbers only grow
		size_t upTo = 0;
		while (upTo < events.size() && events[upTo].seq <= releasedSeq)
			upTo++;
		if (upTo == events.size())
			now.swap(events);
		else
		{
			now.assign(events.begin(), events.begin() + upTo);
			events.erase(events.begin(), events.begin() + upTo);
		}
		eventsWaiting = !events.empty();
	}
	for (const Event& e : now)
	{
		if (e.interrupt)
			__GeTriggerInterrupt(e.id, e.pc, e.ticks);
		else
			__GeTriggerSync(e.type, e.id, e.ticks);
	}
}

u64 busyNs() { return busyTotal; }
u64 cpuNs() { return cpuTotal; }
u64 waitedNs() { return waitedTotal; }

std::string takeWaitSites()
{
	std::vector<WaitSite> sites;
	for (WaitSite &site : waitSites)
	{
		if (site.at != 0)
			sites.push_back(site);
		site = WaitSite();
	}
	std::sort(sites.begin(), sites.end(), [](const WaitSite &a, const WaitSite &b) { return a.ns > b.ns; });
	std::string out;
	for (size_t i = 0; i < sites.size() && i < 5; i++)
	{
		Dl_info info {};
		const uintptr_t base = dladdr((const void *)sites[i].at, &info) ? (uintptr_t)info.dli_fbase : 0;
		char one[64];
		snprintf(one, sizeof(one), "%s0x%lx %.0f ms/%u", out.empty() ? "" : ", ", (unsigned long)(sites[i].at - base), sites[i].ns / 1e6, sites[i].count);
		out += one;
	}
	return out.empty() ? "none" : out;
}

}	// namespace ge
