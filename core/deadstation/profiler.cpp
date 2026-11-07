#include "profiler.h"

#include "Core/CoreTiming.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace deadstation::profiler
{
namespace
{

constexpr int MaxThreads = 16;
constexpr size_t MaxSamples = 1 << 18;
constexpr int StackDepth = 20;

struct Sample
{
	uintptr_t pc, lr;
	uint32_t ms;
	uintptr_t stack[StackDepth];
};

struct ThreadSamples
{
	char name[16];
	timer_t timer;
	uintptr_t stackLo, stackHi;
	std::atomic<size_t> count;
	Sample samples[MaxSamples];
};

const char *const outPath = std::getenv("DEADSTATION_PORTABLE_PROF");
uint64_t startNs;
ThreadSamples *threads[MaxThreads];
std::atomic<int> threadCount;
std::atomic<bool> stopped;
thread_local ThreadSamples *current;

uint64_t nowNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void onSample(int, siginfo_t *, void *context)
{
	ThreadSamples *t = current;
	if (t == nullptr || stopped.load(std::memory_order_relaxed))
		return;
	const size_t i = t->count.load(std::memory_order_relaxed);
	if (i >= MaxSamples)
		return;
	const mcontext_t& mc = static_cast<ucontext_t *>(context)->uc_mcontext;
	Sample& s = t->samples[i];
	s.pc = mc.pc;
	s.lr = mc.regs[30];
	s.ms = (uint32_t)((nowNs() - startNs) / 1000000);
	uintptr_t fp = mc.regs[29];
	int d = 0;
	for (; d < StackDepth; d++)
	{
		if (fp < t->stackLo || fp + 16 > t->stackHi || (fp & 7) != 0)
			break;
		const uintptr_t *frame = (const uintptr_t *)fp;
		s.stack[d] = frame[1];
		if (frame[0] <= fp)
		{
			d++;
			break;
		}
		fp = frame[0];
	}
	for (; d < StackDepth; d++)
		s.stack[d] = 0;
	t->count.store(i + 1, std::memory_order_relaxed);
}

}	// namespace

namespace
{
struct Mark { uint64_t ns; uint64_t ticks; uint32_t a, b; char tag; };
constexpr size_t MaxMarks = 1 << 20;
Mark *marks = nullptr;
std::atomic<size_t> markCount;
const char *const tracePath = std::getenv("DEADSTATION_PORTABLE_TRACE");
}

void trace(char tag, unsigned a, unsigned b)
{
	if (tracePath == nullptr)
		return;
	if (marks == nullptr)
		marks = (Mark *)mmap(nullptr, sizeof(Mark) * MaxMarks, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	const size_t i = markCount.fetch_add(1);
	if (i < MaxMarks)
		marks[i] = { nowNs(), CoreTiming::GetTicks(), a, b, tag };
}

void startThread(const char *name)
{
	if (outPath == nullptr)
		return;
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, [] {
		startNs = nowNs();
		struct sigaction sa {};
		sa.sa_sigaction = onSample;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGPROF, &sa, nullptr);
	});
	const int index = threadCount.fetch_add(1);
	if (index >= MaxThreads)
		return;
	// untouched pages cost nothing: only what a thread samples is real
	void *memory = mmap(nullptr, sizeof(ThreadSamples), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
	{
		std::perror("profiler: mmap");
		return;
	}
	ThreadSamples *t = static_cast<ThreadSamples *>(memory);
	std::strncpy(t->name, name, sizeof(t->name) - 1);
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) == 0)
	{
		void *addr;
		size_t size;
		if (pthread_attr_getstack(&attr, &addr, &size) == 0)
		{
			t->stackLo = (uintptr_t)addr;
			t->stackHi = (uintptr_t)addr + size;
		}
		pthread_attr_destroy(&attr);
	}
	current = t;
	threads[index] = t;
	sigevent sev {};
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = SIGPROF;
	sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
	const clockid_t clock = std::getenv("DEADSTATION_PORTABLE_PROF_WALL") != nullptr ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID;
	if (timer_create(clock, &sev, &t->timer) != 0)
	{
		std::perror("profiler: timer_create");
		return;
	}
	itimerspec every {};
	every.it_interval.tv_nsec = 1000000;
	every.it_value.tv_nsec = 1000000;
	timer_settime(t->timer, 0, &every, nullptr);
}

void dump()
{
	if (tracePath != nullptr && marks != nullptr)
		if (FILE *t = std::fopen(tracePath, "w"))
		{
			const size_t n = std::min(markCount.load(), MaxMarks);
			for (size_t i = 0; i < n; i++)
				std::fprintf(t, "%c %llu %llu %x %x\n", marks[i].tag, (unsigned long long)(marks[i].ns / 1000), (unsigned long long)marks[i].ticks, marks[i].a, marks[i].b);
			std::fclose(t);
		}
	if (outPath == nullptr || stopped.exchange(true))
		return;
	const int count = std::min(threadCount.load(), MaxThreads);
	for (int i = 0; i < count; i++)
		if (threads[i] != nullptr)
			timer_delete(threads[i]->timer);
	FILE *f = std::fopen(outPath, "w");
	if (f == nullptr)
	{
		std::perror(outPath);
		return;
	}
	if (FILE *maps = std::fopen("/proc/self/maps", "r"))
	{
		char line[512];
		while (std::fgets(line, sizeof(line), maps))
			std::fprintf(f, "map %s", line);
		std::fclose(maps);
	}
	// the JIT's code, and its blocks by where they are
	uintptr_t codeStart = 0, codeEnd = 0;
	struct Range { uintptr_t start, end; uint32_t vaddr; };
	std::vector<Range> ranges;
	if (MIPSComp::jit != nullptr)
	{
		codeStart = (uintptr_t)MIPSComp::jit->GetCodeBase() & ~(uintptr_t)0xFFFFFF;
		JitBlockCache *cache = MIPSComp::jit->GetBlockCache();
		if (cache != nullptr)
		{
			uintptr_t lo = UINTPTR_MAX, hi = 0;
			for (int i = 0; i < cache->GetNumBlocks(); i++)
			{
				const JitBlock *b = cache->GetBlock(i);
				if (b->invalid || b->normalEntry == nullptr)
					continue;
				const uintptr_t start = (uintptr_t)b->checkedEntry, end = (uintptr_t)b->normalEntry + b->codeSize;
				ranges.push_back({ start, end, b->originalAddress });
				lo = std::min(lo, start);
				hi = std::max(hi, end);
			}
			if (hi != 0)
			{
				codeStart = lo & ~(uintptr_t)0xFFF;
				codeEnd = hi + 0x100000;
			}
		}
	}
	std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
	std::fprintf(f, "code %zx %zx\n", (size_t)codeStart, (size_t)codeEnd);
	std::map<uint32_t, size_t> hot;
	const auto blockOf = [&](uintptr_t pc) -> uint32_t {
		auto it = std::upper_bound(ranges.begin(), ranges.end(), pc, [](uintptr_t p, const Range& r) { return p < r.start; });
		if (it == ranges.begin())
			return 0;
		--it;
		return pc < it->end ? it->vaddr : 0;
	};
	for (int i = 0; i < count; i++)
	{
		ThreadSamples *t = threads[i];
		if (t == nullptr)
			continue;
		const size_t n = t->count.load();
		for (size_t j = 0; j < n; j++)
		{
			const Sample& s = t->samples[j];
			uint32_t vaddr = 0;
			if (s.pc >= codeStart && s.pc < codeEnd)
			{
				vaddr = blockOf(s.pc);
				hot[vaddr]++;
			}
			std::fprintf(f, "s %s %zx %zx %x 0 %u", t->name, (size_t)s.pc, (size_t)s.lr, vaddr, s.ms);
			for (int d = 0; d < StackDepth && s.stack[d] != 0; d++)
				std::fprintf(f, " %zx", (size_t)s.stack[d]);
			std::fprintf(f, "\n");
		}
	}
	// the hottest blocks: guest words and host bytes, to see what in them takes the time
	{
		std::vector<std::pair<size_t, uint32_t>> order;
		for (const auto& [vaddr, n] : hot)
			if (vaddr != 0)
				order.push_back({ n, vaddr });
		std::sort(order.rbegin(), order.rend());
		if (order.size() > 400)
			order.resize(400);
		JitBlockCache *cache = MIPSComp::jit != nullptr ? MIPSComp::jit->GetBlockCache() : nullptr;
		if (cache != nullptr)
			for (int i = 0; i < cache->GetNumBlocks(); i++)
			{
				const JitBlock *b = cache->GetBlock(i);
				if (b->invalid || b->normalEntry == nullptr)
					continue;
				if (std::none_of(order.begin(), order.end(), [&](const auto& o) { return o.second == b->originalAddress; }))
					continue;
				std::fprintf(f, "block %x %zx %u %u ", b->originalAddress, (size_t)b->checkedEntry, (unsigned)(b->normalEntry + b->codeSize - b->checkedEntry), (unsigned)b->originalSize);
				for (const u8 *p = b->checkedEntry; p < b->normalEntry + b->codeSize; p++)
					std::fprintf(f, "%02x", *p);
				std::fprintf(f, "\n");
				std::fprintf(f, "mips %x", b->originalAddress);
				for (unsigned w = 0; w < b->originalSize; w++)
					std::fprintf(f, " %08x", Memory::ReadUnchecked_Instruction(b->originalAddress + w * 4).encoding);
				std::fprintf(f, "\n");
			}
	}
	std::fclose(f);
	std::printf("profiler: samples in %s\n", outPath);
	std::fflush(stdout);
}

}	// namespace deadstation::profiler
