#include "jitcache.h"

#include "Common/CommonTypes.h"
#include "Core/MemMap.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace deadstation::jitcache
{

namespace
{

struct Entry
{
	u32 address;
	u32 words;
	u32 hash;
};

std::string filePath;
std::vector<Entry> entries;
size_t cursor = 0;
int sweepWait = 0;

// the guest's code as it is, whatever blocks have been made of it: an entry is only compiled where it's still the same code
u32 hashCode(u32 address, u32 words)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < words; i++)
		h = (h ^ Memory::Read_Instruction(address + i * 4).encoding) * 16777619u;
	return h;
}

bool valid(const Entry& e)
{
	return e.words != 0 && Memory::IsValid4AlignedAddress(e.address) && Memory::IsValid4AlignedAddress(e.address + (e.words - 1) * 4);
}

double nowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void run(double budgetMs)
{
	if (MIPSComp::jit == nullptr || entries.empty())
		return;
	JitBlockCache *blocks = MIPSComp::jit->GetBlockCache();
	if (blocks == nullptr)
		return;
	const double until = nowMs() + budgetMs;
	while (cursor < entries.size() && blocks->GetNumBlocks() < 150000)
	{
		const Entry& e = entries[cursor++];
		if (valid(e) && blocks->GetBlockNumberFromStartAddress(e.address) < 0 && (Memory::ReadUnchecked_U32(e.address) & MIPS_EMUHACK_MASK) != MIPS_EMUHACK_OPCODE && hashCode(e.address, e.words) == e.hash)
			MIPSComp::jit->Compile(e.address);
		if ((cursor & 7) == 0 && nowMs() >= until)
			return;
	}
}

}	// namespace

void start(const std::string& gameId)
{
	filePath = gameId;
	entries.clear();
	cursor = 0;
	std::ifstream in(filePath, std::ios::binary);
	Entry e;
	while (in.read((char *)&e, sizeof(e)))
		entries.push_back(e);
}

void tick(double budgetMs)
{
	if (cursor >= entries.size())
	{
		// Code comes and goes with the game's overlays: now and then what wasn't there gets another look.
		if (++sweepWait < 300)
			return;
		sweepWait = 0;
		cursor = 0;
	}
	run(budgetMs);
}

void afterLoad()
{
	cursor = 0;
	run(250.0);
}

void finish()
{
	if (filePath.empty() || MIPSComp::jit == nullptr)
		return;
	JitBlockCache *blocks = MIPSComp::jit->GetBlockCache();
	if (blocks == nullptr)
		return;
	std::unordered_set<u32> seen;
	std::vector<Entry> out;
	out.reserve(entries.size() + blocks->GetNumBlocks());
	// What was known stays in its order, with what's new behind it: the order they're compiled in is the order they lie in memory.
	const int numBlocks = blocks->GetNumBlocks();
	std::vector<Entry> fresh;
	for (int i = 0; i < numBlocks; i++)
	{
		const JitBlock *b = blocks->GetBlock(i);
		if (b->invalid || b->originalSize == 0)
			continue;
		Entry e { b->originalAddress, b->originalSize, 0 };
		if (!valid(e))
			continue;
		e.hash = hashCode(e.address, e.words);
		fresh.push_back(e);
	}
	std::unordered_map<u32, const Entry *> freshByAddress;
	for (const Entry& e : fresh)
		freshByAddress[e.address] = &e;
	for (const Entry& e : entries)
		if (seen.insert(e.address).second)
		{
			// its code as it's now, if it's been compiled this time
			const auto it = freshByAddress.find(e.address);
			out.push_back(it != freshByAddress.end() ? *it->second : e);
		}
	for (const Entry& e : fresh)
		if (seen.insert(e.address).second)
			out.push_back(e);
	const std::string tmp = filePath + ".tmp";
	std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
	if (!out.empty())
		file.write((const char *)out.data(), out.size() * sizeof(Entry));
	file.close();
	if (file.good())
		std::rename(tmp.c_str(), filePath.c_str());
	else
		std::remove(tmp.c_str());
	filePath.clear();
}

}
