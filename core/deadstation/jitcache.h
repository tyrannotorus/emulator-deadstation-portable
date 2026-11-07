// The game's translated blocks, remembered between sessions: a fight's first seconds compiled up to 5% of the CPU's frame on the spot.
#pragma once

#include <string>

namespace deadstation::jitcache
{

void start(const std::string& gameId);
// Between frames, on the CPU's thread: some of what's known and is in memory now, compiled ahead of time.
void tick(double budgetMs);
// A state's been loaded: the JIT is empty, and it's a moment nobody minds waiting.
void afterLoad();
// The game's ending: while the JIT and guest memory are still there.
void finish();

}
