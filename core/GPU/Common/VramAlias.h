// Textures on their way into VRAM. Games keep their textures in RAM and block-transfer each one into a scratch area of VRAM just
// before drawing with it: God of War, 350 times a frame, 1.5 MB, each then hashed where it landed. While a list runs, nothing can
// tell a transfer from RAM has not happened yet if whoever reads the destination reads the source instead. So those transfers are
// noted here; the texture cache reads through the note, and the entry it finds is the RAM texture's, the same every frame. Only what
// is left in VRAM at the list's end is really copied, as the game may look at it then. Anything else that reads VRAM copies first.
// The GE's thread only.
#pragma once
#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Core/MemMap.h"

#if PPSSPP_ARCH(ARM64_NEON)
#include <arm_neon.h>
#endif

#include <cstring>
#include <vector>

class VramAlias
{
public:
	enum class Read { Plain, Source, Conflict };

	bool pending() const { return !notes_.empty(); }

	void note(u32 dst, u32 src, u32 size)
	{
		// Reads mostly hit the same note, in turn, thousands of times a frame: it's the one that has to know when it's overwritten.
		if (memo_ < notes_.size() && overlaps(notes_[memo_], dst, size))
			memo_ = NO_MEMO;
		notes_.push_back({ dst, src, size });
		if (dst < lo_)
			lo_ = dst;
		if (dst + size > hi_)
			hi_ = dst + size;
	}

	// Where a display list at addr runs from: a list that has just been transferred runs from where it came, and copying it is saved.
	u32 listAddress(u32 addr)
	{
		if (notes_.empty() || !Memory::IsVRAMAddress(addr))
			return addr;
		const u32 a = addr & 0x3FFFFFFF;
		const ptrdiff_t i = newestHolding(a);
		return i < 0 ? addr : notes_[i].src + (a - notes_[i].dst);
	}

	// where a read of [addr, addr + size) should go: Plain, VRAM as it is, nothing noted touches it; Source, *out in RAM; Conflict, only
	// part of it is noted: materialize first
	Read resolve(u32 addr, u32 size, u32 *out) const
	{
		addr &= 0x3FFFFFFF;
		if ((addr & 0x00600000) != 0)
			return Read::Conflict;	// a VRAM mirror
		// past every note's end or before every start: none overlaps it (overlaps() needs addr < an end, addr + size > a start)
		if (addr >= hi_ || addr + size <= lo_)
			return Read::Plain;
		if (memo_ < notes_.size())
		{
			const Note& n = notes_[memo_];
			if (addr >= n.dst && addr + size <= n.dst + n.size)
			{
				*out = n.src + (addr - n.dst);
				return Read::Source;
			}
		}
		for (size_t i = notes_.size(); i-- > 0; )
		{
			const Note& n = notes_[i];
			if (overlaps(n, addr, size))
			{
				if (addr >= n.dst && addr + size <= n.dst + n.size)
				{
					*out = n.src + (addr - n.dst);
					// only a note nothing newer has touched can answer the next read by itself
					size_t newer = i + 1;
					while (newer < notes_.size() && !overlaps(n, notes_[newer].dst, notes_[newer].size))
						newer++;
					memo_ = newer == notes_.size() ? i : NO_MEMO;
					return Read::Source;
				}
				return Read::Conflict;
			}
		}
		return Read::Plain;
	}

	// What is left of the notes, copied: only the bytes the newest note over them says. The textures overlap each other in a scratch area,
	// and copying each that wasn't wholly overwritten by a single later one was most of God of War's 1.5 MB, every list.
	void materialize()
	{
		if (notes_.empty())
			return;
		covered_.clear();
		for (size_t i = notes_.size(); i-- > 0; )
		{
			const Note& n = notes_[i];
			const u32 end = n.dst + n.size;
			size_t at = 0;
			while (at < covered_.size() && covered_[at].end < n.dst)
				at++;
			const size_t first = at;
			u32 cursor = n.dst;
			Span merged { n.dst, end };
			for (; at < covered_.size() && covered_[at].start <= end; at++)
			{
				const Span& c = covered_[at];
				if (c.start > cursor)
					copyPart(n, cursor, c.start - cursor);
				if (c.end > cursor)
					cursor = c.end;
				if (c.start < merged.start)
					merged.start = c.start;
				if (c.end > merged.end)
					merged.end = c.end;
			}
			if (cursor < end)
				copyPart(n, cursor, end - cursor);
			if (first == at)
				covered_.insert(covered_.begin() + first, merged);
			else
			{
				covered_[first] = merged;
				covered_.erase(covered_.begin() + first + 1, covered_.begin() + at);
			}
		}
		notes_.clear();
		memo_ = NO_MEMO;
		lo_ = 0xFFFFFFFF;
		hi_ = 0;
	}

	// Where to read [addr, addr + size) from, whatever is noted: God of War's vertices come through VRAM too, most of the 1.5 MB
	u32 readable(u32 addr, u32 size)
	{
		if (notes_.empty() || !Memory::IsVRAMAddress(addr))
			return addr;
		u32 source = 0;
		const Read read = resolve(addr, size, &source);
		if (read == Read::Source)
			return source;
		if (read == Read::Conflict)
			materialize();
		return addr;
	}

	// Something else is about to read from addr, a display list or vertices: God of War transfers small lists into VRAM and calls them.
	// Only what is noted over there has to be real, and most VRAM isn't noted at all.
	void materializeIfVRAM(u32 addr)
	{
		if (notes_.empty() || !Memory::IsVRAMAddress(addr))
			return;
		addr &= 0x3FFFFFFF;
		u32 lo = 0, hi = 0;
		const ptrdiff_t newest = newestHolding(addr);
		if (newest >= 0)
		{
			lo = notes_[newest].dst;
			hi = lo + notes_[newest].size;
		}
		if (hi == 0)
			return;
		size_t kept = 0;
		for (size_t i = 0; i < notes_.size(); i++)
		{
			const Note n = notes_[i];
			if (n.dst < hi && n.dst + n.size > lo)
				memcpy(Memory::GetPointerWriteUnchecked(n.dst), Memory::GetPointerUnchecked(n.src), n.size);
			else
				notes_[kept++] = n;
		}
		notes_.resize(kept);
	}

private:
	struct Note { u32 dst, src, size; };
	struct Span { u32 start, end; };
	static constexpr size_t NO_MEMO = (size_t)-1;

	static bool overlaps(const Note& n, u32 addr, u32 size) { return addr < n.dst + n.size && addr + size > n.dst; }

	// The newest note with dst <= a < dst + size (u32 sums, as everywhere here), or -1. None can hold an a outside [lo_, hi_).
	// Four notes at a time where NEON is: the same two compares a note, the newest of a group's hits.
	ptrdiff_t newestHolding(u32 a) const
	{
		if (a < lo_ || a >= hi_)
			return -1;
		size_t i = notes_.size();
#if PPSSPP_ARCH(ARM64_NEON)
		static_assert(sizeof(Note) == 12, "notes as three words");
		const uint32x4_t va = vdupq_n_u32(a);
		for (; i >= 4; i -= 4)
		{
			const uint32x4x3_t n = vld3q_u32(&notes_[i - 4].dst);	// dst, src, size of four notes
			const uint32x4_t hit = vandq_u32(vcgeq_u32(va, n.val[0]), vcltq_u32(va, vaddq_u32(n.val[0], n.val[2])));
			const uint64_t bits = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(hit)), 0);
			if (bits)
				return (ptrdiff_t)(i - 4) + (63 - __builtin_clzll(bits)) / 16;
		}
#endif
		while (i-- > 0)
			if (a >= notes_[i].dst && a < notes_[i].dst + notes_[i].size)
				return (ptrdiff_t)i;
		return -1;
	}

	static void copyPart(const Note& n, u32 at, u32 size)
	{
		memcpy(Memory::GetPointerWriteUnchecked(at), Memory::GetPointerUnchecked(n.src + (at - n.dst)), size);
	}

	std::vector<Note> notes_;
	std::vector<Span> covered_;	// sorted, none touching
	mutable size_t memo_ = NO_MEMO;
	// every note's dst at or above lo_, every dst + size at or below hi_ (kept until the notes are cleared: a note dropped by
	// materializeIfVRAM leaves them wider than they need be, never narrower)
	u32 lo_ = 0xFFFFFFFF, hi_ = 0;
};

extern VramAlias g_vramAlias;
