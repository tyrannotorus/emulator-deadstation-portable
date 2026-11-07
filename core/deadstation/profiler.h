// A sampling profiler, on with DEADSTATION_PORTABLE_PROF=<file>: each thread that registers is interrupted every millisecond of
// its own CPU time (with DEADSTATION_PORTABLE_PROF_WALL, of wall time, so that waits show), and where it was is noted, with
// its return addresses. dump() writes it all, with which guest block the samples in JIT code were in (local/brick/profile.py).
#pragma once

namespace deadstation::profiler
{

void startThread(const char *name);
void dump();
// the timeline: a mark, with two values, at this moment (DEADSTATION_PORTABLE_TRACE=<file>)
void trace(char tag, unsigned a = 0, unsigned b = 0);

}
