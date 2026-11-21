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

#include <cstring>
#include <type_traits>

#include "ppsspp_config.h"

#include "Common/Math/SIMDHeaders.h"
#include "GPU/Common/IndexGenerator.h"

// Points don't need indexing...
const u8 IndexGenerator::indexedPrimitiveType[7] = {
	GE_PRIM_POINTS,
	GE_PRIM_LINES,
	GE_PRIM_LINES,
	GE_PRIM_TRIANGLES,
	GE_PRIM_TRIANGLES,
	GE_PRIM_TRIANGLES,
	GE_PRIM_RECTANGLES,
};

void IndexGenerator::AddPrim(int prim, int vertexCount, int indexOffset, bool clockwise) {
	switch (prim) {
	case GE_PRIM_POINTS: AddPoints(vertexCount, indexOffset); break;
	case GE_PRIM_LINES: AddLineList(vertexCount, indexOffset); break;
	case GE_PRIM_LINE_STRIP: AddLineStrip(vertexCount, indexOffset); break;
	case GE_PRIM_TRIANGLES: AddList(vertexCount, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_STRIP: AddStrip(vertexCount, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_FAN: AddFan(vertexCount, indexOffset, clockwise); break;
	case GE_PRIM_RECTANGLES: AddRectangles(vertexCount, indexOffset); break;  // Same
	}
}

void IndexGenerator::AddPoints(int numVerts, int indexOffset) {
	u16 *outInds = inds_;
	for (int i = 0; i < numVerts; i++)
		*outInds++ = indexOffset + i;
	inds_ = outInds;
}

void IndexGenerator::AddList(int numVerts, int indexOffset, bool clockwise) {
	u16 *outInds = inds_;
	const int v1 = clockwise ? 1 : 2;
	const int v2 = clockwise ? 2 : 1;
	for (int i = 0; i < numVerts; i += 3) {
		*outInds++ = indexOffset + i;
		*outInds++ = indexOffset + i + v1;
		*outInds++ = indexOffset + i + v2;
	}
	inds_ = outInds;
}

alignas(16) static const u16 offsets_clockwise[24] = {
	0, (u16)(0 + 1), (u16)(0 + 2),
	(u16)(1 + 1), 1, (u16)(1 + 2),
	2, (u16)(2 + 1), (u16)(2 + 2),
	(u16)(3 + 1), 3, (u16)(3 + 2),
	4, (u16)(4 + 1), (u16)(4 + 2),
	(u16)(5 + 1), 5, (u16)(5 + 2),
	6, (u16)(6 + 1), (u16)(6 + 2),
	(u16)(7 + 1), 7, (u16)(7 + 2),
};

alignas(16) static const uint16_t offsets_counter_clockwise[24] = {
	0, (u16)(0 + 2), (u16)(0 + 1),
	1, (u16)(1 + 1), (u16)(1 + 2),
	2, (u16)(2 + 2), (u16)(2 + 1),
	3, (u16)(3 + 1), (u16)(3 + 2),
	4, (u16)(4 + 2), (u16)(4 + 1),
	5, (u16)(5 + 1), (u16)(5 + 2),
	6, (u16)(6 + 2), (u16)(6 + 1),
	7, (u16)(7 + 1), (u16)(7 + 2),
};

void IndexGenerator::AddStrip(int numVerts, int indexOffset, bool clockwise) {
	int numTris = numVerts - 2;
	if (numTris <= 0) {
		return;
	}
	uint16x8_t ibase8 = vdupq_n_u16(indexOffset);
	const u16 *offsets = clockwise ? offsets_clockwise : offsets_counter_clockwise;
	u16 *dst = inds_;
	uint16x8_t offsets0 = vaddq_u16(ibase8, vld1q_u16(offsets));
	vst1q_u16(dst, offsets0);
	if (numTris > 2) {
		uint16x8_t offsets1 = vaddq_u16(ibase8, vld1q_u16(offsets + 8));
		vst1q_u16(dst + 8, offsets1);
		if (numTris > 5) {
			uint16x8_t offsets2 = vaddq_u16(ibase8, vld1q_u16(offsets + 16));
			vst1q_u16(dst + 16, offsets2);
			uint16x8_t increment = vdupq_n_u16(8);
			int numChunks = (numTris + 7) >> 3;
			for (int i = 1; i < numChunks; i++) {
				dst += 3 * 8;
				offsets0 = vaddq_u16(offsets0, increment);
				offsets1 = vaddq_u16(offsets1, increment);
				offsets2 = vaddq_u16(offsets2, increment);
				vst1q_u16(dst, offsets0);
				vst1q_u16(dst + 8, offsets1);
				vst1q_u16(dst + 16, offsets2);
			}
		}
	}
	inds_ += numTris * 3;
}

// God of War uses this for text. Otherwise rare, not much reason to optimize.
void IndexGenerator::AddFan(int numVerts, int indexOffset, bool clockwise) {
	const int numTris = numVerts - 2;
	u16 *outInds = inds_;
	const int v1 = clockwise ? 1 : 2;
	const int v2 = clockwise ? 2 : 1;
	for (int i = 0; i < numTris; i++) {
		*outInds++ = indexOffset;
		*outInds++ = indexOffset + i + v1;
		*outInds++ = indexOffset + i + v2;
	}
	inds_ = outInds;
}

//Lines
void IndexGenerator::AddLineList(int numVerts, int indexOffset) {
	u16 *outInds = inds_;
	numVerts &= ~1;
	for (int i = 0; i < numVerts; i += 2) {
		*outInds++ = indexOffset + i;
		*outInds++ = indexOffset + i + 1;
	}
	inds_ = outInds;
}

void IndexGenerator::AddLineStrip(int numVerts, int indexOffset) {
	const int numLines = numVerts - 1;
	u16 *outInds = inds_;
	for (int i = 0; i < numLines; i++) {
		*outInds++ = indexOffset + i;
		*outInds++ = indexOffset + i + 1;
	}
	inds_ = outInds;
}

void IndexGenerator::AddRectangles(int numVerts, int indexOffset) {
	u16 *outInds = inds_;
	//rectangles always need 2 vertices, disregard the last one if there's an odd number
	numVerts = numVerts & ~1;
	for (int i = 0; i < numVerts; i += 2) {
		*outInds++ = indexOffset + i;
		*outInds++ = indexOffset + i + 1;
	}
	inds_ = outInds;
}

template <class ITypeLE>
void IndexGenerator::TranslatePoints(int numInds, const ITypeLE *inds, int indexOffset) {
	u16 *outInds = inds_;
	for (int i = 0; i < numInds; i++)
		*outInds++ = indexOffset + inds[i];
	inds_ = outInds;
}

template <class ITypeLE>
void IndexGenerator::TranslateLineList(int numInds, const ITypeLE *inds, int indexOffset) {
	u16 *outInds = inds_;
	numInds = numInds & ~1;
	for (int i = 0; i < numInds; i += 2) {
		*outInds++ = indexOffset + inds[i];
		*outInds++ = indexOffset + inds[i + 1];
	}
	inds_ = outInds;
}

template <class ITypeLE>
void IndexGenerator::TranslateLineStrip(int numInds, const ITypeLE *inds, int indexOffset) {
	int numLines = numInds - 1;
	u16 *outInds = inds_;
	for (int i = 0; i < numLines; i++) {
		*outInds++ = indexOffset + inds[i];
		*outInds++ = indexOffset + inds[i + 1];
	}
	inds_ = outInds;
}

#if PPSSPP_ARCH(ARM64_NEON)
namespace {
// Deadstation Portable: a strip's triangles eight at a time from an even one, as TranslateStrip makes them: triangle t's corners
// (t, t + w, t + 3 - w), w the first wind (1 clockwise, 2 not) for an even t and 3 - it for an odd one. As byte indices into
// the indices loaded from the first corner: two bytes each for 16-bit indices, one for 8-bit.
struct StripTables {
	u8 u16Wind[2][48];
	u8 u8Wind[2][24];
};
constexpr StripTables MakeStripTables() {
	StripTables tables{};
	for (int first = 1; first <= 2; first++) {
		for (int t = 0; t < 8; t++) {
			const int w = (t & 1) ? 3 - first : first;
			const int corner[3] = { t, t + w, t + 3 - w };
			for (int k = 0; k < 3; k++) {
				tables.u16Wind[first - 1][(t * 3 + k) * 2] = (u8)(corner[k] * 2);
				tables.u16Wind[first - 1][(t * 3 + k) * 2 + 1] = (u8)(corner[k] * 2 + 1);
				tables.u8Wind[first - 1][t * 3 + k] = (u8)corner[k];
			}
		}
	}
	return tables;
}
alignas(16) constexpr StripTables stripTables = MakeStripTables();
}  // namespace
#endif

template <class ITypeLE>
void IndexGenerator::TranslateList(int numInds, const ITypeLE *inds, int indexOffset, bool clockwise) {
	// We only bother doing this minor optimization in triangle list, since it's by far the most
	// common operation that can benefit.
	if (sizeof(ITypeLE) == sizeof(inds_[0]) && indexOffset == 0 && clockwise) {
		memcpy(inds_, inds, numInds * sizeof(ITypeLE));
		inds_ += numInds;
	} else {
		u16 *outInds = inds_;
		int numTris = numInds / 3;  // Round to whole triangles
		numInds = numTris * 3;
		const int v1 = clockwise ? 1 : 2;
		const int v2 = clockwise ? 2 : 1;
		int i = 0;
#if PPSSPP_ARCH(ARM64_NEON)
		// Deadstation Portable: eight triangles at a time (clockwise as they are, not with corners 1 and 2 exchanged), in 16
		// bits as the stores below wrap them. The scalar loop's stores could have been changing its next indices for all the
		// compiler knew, so it ran an index at a time.
		const uint16x8_t off = vdupq_n_u16((u16)indexOffset);
		if (std::is_same<ITypeLE, u16_le>::value || std::is_same<ITypeLE, u8>::value) {
			if (clockwise) {
				// 24 indices a step: the scalar loop then starts on a triangle
				for (; i + 24 <= numInds; i += 24, outInds += 24) {
					for (int k = 0; k < 24; k += 8) {
						uint16x8_t v;
						if (std::is_same<ITypeLE, u8>::value)
							v = vmovl_u8(vld1_u8((const u8 *)(inds + i + k)));
						else
							v = vld1q_u16((const u16 *)(inds + i + k));
						vst1q_u16(outInds + k, vaddq_u16(v, off));
					}
				}
			} else {
				for (; i + 24 <= numInds; i += 24, outInds += 24) {
					uint16x8x3_t t;
					if (std::is_same<ITypeLE, u8>::value) {
						const uint8x8x3_t b = vld3_u8((const u8 *)(inds + i));
						t.val[0] = vmovl_u8(b.val[0]);
						t.val[1] = vmovl_u8(b.val[2]);
						t.val[2] = vmovl_u8(b.val[1]);
					} else {
						const uint16x8x3_t h = vld3q_u16((const u16 *)(inds + i));
						t.val[0] = h.val[0];
						t.val[1] = h.val[2];
						t.val[2] = h.val[1];
					}
					t.val[0] = vaddq_u16(t.val[0], off);
					t.val[1] = vaddq_u16(t.val[1], off);
					t.val[2] = vaddq_u16(t.val[2], off);
					vst3q_u16(outInds, t);
				}
			}
		}
#endif
		for (; i < numInds; i += 3) {
			*outInds++ = indexOffset + inds[i];
			*outInds++ = indexOffset + inds[i + v1];
			*outInds++ = indexOffset + inds[i + v2];
		}
		inds_ = outInds;
	}
}

template <class ITypeLE>
void IndexGenerator::TranslateStrip(int numInds, const ITypeLE *inds, int indexOffset, bool clockwise) {
	int wind = clockwise ? 1 : 2;
	int numTris = numInds - 2;
	u16 *outInds = inds_;
	int i = 0;
#if PPSSPP_ARCH(ARM64_NEON)
	// Deadstation Portable: eight triangles at a time (each step starts on an even one, so the wind is the first's), their 24
	// corners picked from the 16 indices loaded by a table lookup, in 16 bits as the stores below wrap them. Only while all 16 are
	// the draw's (the 8 triangles need 10).
	if (std::is_same<ITypeLE, u16_le>::value || std::is_same<ITypeLE, u8>::value) {
		const uint16x8_t off = vdupq_n_u16((u16)indexOffset);
		if (std::is_same<ITypeLE, u8>::value) {
			const u8 *tab = stripTables.u8Wind[wind - 1];
			const uint8x16_t t0 = vld1q_u8(tab);
			const uint8x8_t t1 = vld1_u8(tab + 16);
			for (; i + 16 <= numInds; i += 8, outInds += 24) {
				const uint8x16_t src = vld1q_u8((const u8 *)(inds + i));
				const uint8x16_t a = vqtbl1q_u8(src, t0);
				const uint8x8_t b = vqtbl1_u8(src, t1);
				vst1q_u16(outInds, vaddq_u16(vmovl_u8(vget_low_u8(a)), off));
				vst1q_u16(outInds + 8, vaddq_u16(vmovl_high_u8(a), off));
				vst1q_u16(outInds + 16, vaddq_u16(vmovl_u8(b), off));
			}
		} else {
			const u8 *tab = stripTables.u16Wind[wind - 1];
			const uint8x16_t t0 = vld1q_u8(tab), t1 = vld1q_u8(tab + 16), t2 = vld1q_u8(tab + 32);
			for (; i + 16 <= numInds; i += 8, outInds += 24) {
				const uint8x16x2_t src = { { vld1q_u8((const u8 *)(inds + i)), vld1q_u8((const u8 *)(inds + i + 8)) } };
				vst1q_u16(outInds, vaddq_u16(vreinterpretq_u16_u8(vqtbl2q_u8(src, t0)), off));
				vst1q_u16(outInds + 8, vaddq_u16(vreinterpretq_u16_u8(vqtbl2q_u8(src, t1)), off));
				vst1q_u16(outInds + 16, vaddq_u16(vreinterpretq_u16_u8(vqtbl2q_u8(src, t2)), off));
			}
		}
	}
#endif
	for (; i < numTris; i++) {
		*outInds++ = indexOffset + inds[i];
		*outInds++ = indexOffset + inds[i + wind];
		wind ^= 3;  // Toggle between 1 and 2
		*outInds++ = indexOffset + inds[i + wind];
	}
	inds_ = outInds;
}

template <class ITypeLE>
void IndexGenerator::TranslateFan(int numInds, const ITypeLE *inds, int indexOffset, bool clockwise) {
	if (numInds <= 0) return;
	int numTris = numInds - 2;
	u16 *outInds = inds_;
	const int v1 = clockwise ? 1 : 2;
	const int v2 = clockwise ? 2 : 1;
	for (int i = 0; i < numTris; i++) {
		*outInds++ = indexOffset + inds[0];
		*outInds++ = indexOffset + inds[i + v1];
		*outInds++ = indexOffset + inds[i + v2];
	}
	inds_ = outInds;
}

template <class ITypeLE>
inline void IndexGenerator::TranslateRectangles(int numInds, const ITypeLE *inds, int indexOffset) {
	u16 *outInds = inds_;
	//rectangles always need 2 vertices, disregard the last one if there's an odd number
	numInds = numInds & ~1;
	for (int i = 0; i < numInds; i += 2) {
		*outInds++ = indexOffset + inds[i];
		*outInds++ = indexOffset + inds[i+1];
	}
	inds_ = outInds;
}

// Could template this too, but would have to define in header.
void IndexGenerator::TranslatePrim(int prim, int numInds, const u8 *inds, int indexOffset, bool clockwise) {
	switch (prim) {
	case GE_PRIM_POINTS: TranslatePoints<u8>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINES: TranslateLineList<u8>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINE_STRIP: TranslateLineStrip<u8>(numInds, inds, indexOffset); break;
	case GE_PRIM_TRIANGLES: TranslateList<u8>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_STRIP: TranslateStrip<u8>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_FAN: TranslateFan<u8>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_RECTANGLES: TranslateRectangles<u8>(numInds, inds, indexOffset); break;  // Same
	}
}

void IndexGenerator::TranslatePrim(int prim, int numInds, const u16_le *inds, int indexOffset, bool clockwise) {
	switch (prim) {
	case GE_PRIM_POINTS: TranslatePoints<u16_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINES: TranslateLineList<u16_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINE_STRIP: TranslateLineStrip<u16_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_TRIANGLES: TranslateList<u16_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_STRIP: TranslateStrip<u16_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_FAN: TranslateFan<u16_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_RECTANGLES: TranslateRectangles<u16_le>(numInds, inds, indexOffset); break;  // Same
	}
}

void IndexGenerator::TranslatePrim(int prim, int numInds, const u32_le *inds, int indexOffset, bool clockwise) {
	switch (prim) {
	case GE_PRIM_POINTS: TranslatePoints<u32_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINES: TranslateLineList<u32_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_LINE_STRIP: TranslateLineStrip<u32_le>(numInds, inds, indexOffset); break;
	case GE_PRIM_TRIANGLES: TranslateList<u32_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_STRIP: TranslateStrip<u32_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_TRIANGLE_FAN: TranslateFan<u32_le>(numInds, inds, indexOffset, clockwise); break;
	case GE_PRIM_RECTANGLES: TranslateRectangles<u32_le>(numInds, inds, indexOffset); break;  // Same
	}
}
