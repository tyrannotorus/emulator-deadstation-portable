// Copyright (c) 2015- PPSSPP Project.

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

#include "ppsspp_config.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Common/Data/Convert/SmallDataConvert.h"
#include "Common/Common.h"
#include "Common/CPUDetect.h"
#include "Common/Math/SIMDHeaders.h"

void ConvertBGRA8888ToRGBA8888(u32 *dst, const u32 *src, u32 numPixels) {
	u32 i = 0;
	for (; i < numPixels; i++) {
		const u32 c = src[i];
		dst[i] = ((c >> 16) & 0x000000FF) |
			(c & 0xFF00FF00) |
			((c << 16) & 0x00FF0000);
	}
}

void ConvertBGRA8888ToRGB888(u8 *dst, const u32 *src, u32 numPixels) {
	for (uint32_t x = 0; x < numPixels; ++x) {
		uint32_t c = src[x];
		dst[x * 3 + 0] = (c >> 16) & 0xFF;
		dst[x * 3 + 1] = (c >> 8) & 0xFF;
		dst[x * 3 + 2] = (c >> 0) & 0xFF;
	}
}


void ConvertRGBA8888ToRGBA5551(u16 *dst, const u32 *src, u32 numPixels) {
	u32 i = 0;
	for (; i < numPixels; i++) {
		dst[i] = RGBA8888toRGBA5551(src[i]);
	}
}


void ConvertBGRA8888ToRGBA5551(u16 *dst, const u32 *src, u32 numPixels) {
	u32 i = 0;
	for (; i < numPixels; i++) {
		dst[i] = BGRA8888toRGBA5551(src[i]);
	}
}

void ConvertBGRA8888ToRGB565(u16 *dst, const u32 *src, u32 numPixels) {
	for (u32 i = 0; i < numPixels; i++) {
		dst[i] = BGRA8888toRGB565(src[i]);
	}
}

void ConvertBGRA8888ToRGBA4444(u16 *dst, const u32 *src, u32 numPixels) {
	for (u32 i = 0; i < numPixels; i++) {
		dst[i] = BGRA8888toRGBA4444(src[i]);
	}
}

void ConvertRGBA8888ToRGB565(u16 *dst, const u32 *src, u32 numPixels) {
	for (u32 x = 0; x < numPixels; ++x) {
		dst[x] = RGBA8888toRGB565(src[x]);
	}
}

void ConvertRGBA8888ToRGBA4444(u16 *dst, const u32 *src, u32 numPixels) {
	for (u32 x = 0; x < numPixels; ++x) {
		dst[x] = RGBA8888toRGBA4444(src[x]);
	}
}

void ConvertRGBA8888ToRGB888(u8 *dst, const u32 *src, u32 numPixels) {
	for (uint32_t x = 0; x < numPixels; ++x) {
		memcpy(dst + x * 3, src + x, 3);
	}
}

void ConvertRGB565ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u32 i = 0;

	u8 *dst = (u8 *)dst32;
	for (u32 x = i; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert5To8((col) & 0x1f);
		dst[x * 4 + 1] = Convert6To8((col >> 5) & 0x3f);
		dst[x * 4 + 2] = Convert5To8((col >> 11) & 0x1f);
		dst[x * 4 + 3] = 255;
	}
}

void ConvertRGBA5551ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u32 i = 0;

	u8 *dst = (u8 *)dst32;
	for (u32 x = i; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert5To8((col) & 0x1f);
		dst[x * 4 + 1] = Convert5To8((col >> 5) & 0x1f);
		dst[x * 4 + 2] = Convert5To8((col >> 10) & 0x1f);
		dst[x * 4 + 3] = (col >> 15) ? 255 : 0;
	}
}

void ConvertRGBA4444ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u32 i = 0;

	u8 *dst = (u8 *)dst32;
	for (u32 x = i; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert4To8(col & 0xf);
		dst[x * 4 + 1] = Convert4To8((col >> 4) & 0xf);
		dst[x * 4 + 2] = Convert4To8((col >> 8) & 0xf);
		dst[x * 4 + 3] = Convert4To8(col >> 12);
	}
}

void ConvertBGR565ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u8 *dst = (u8 *)dst32;
	for (u32 x = 0; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert5To8((col >> 11) & 0x1f);
		dst[x * 4 + 1] = Convert6To8((col >> 5) & 0x3f);
		dst[x * 4 + 2] = Convert5To8((col) & 0x1f);
		dst[x * 4 + 3] = 255;
	}
}

void ConvertABGR1555ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u8 *dst = (u8 *)dst32;
	for (u32 x = 0; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert5To8((col >> 11) & 0x1f);
		dst[x * 4 + 1] = Convert5To8((col >> 6) & 0x1f);
		dst[x * 4 + 2] = Convert5To8((col >> 1) & 0x1f);
		dst[x * 4 + 3] = (col & 1) ? 255 : 0;
	}
}

void ConvertABGR4444ToRGBA8888(u32 *dst32, const u16 *src, u32 numPixels) {
	u8 *dst = (u8 *)dst32;
	for (u32 x = 0; x < numPixels; x++) {
		u16 col = src[x];
		dst[x * 4] = Convert4To8(col >> 12);
		dst[x * 4 + 1] = Convert4To8((col >> 8) & 0xf);
		dst[x * 4 + 2] = Convert4To8((col >> 4) & 0xf);
		dst[x * 4 + 3] = Convert4To8(col & 0xf);
	}
}

void ConvertRGBA4444ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels) {
	for (u32 x = 0; x < numPixels; x++) {
		u16 c = src[x];
		u32 r = Convert4To8(c & 0x000f);
		u32 g = Convert4To8((c >> 4) & 0x000f);
		u32 b = Convert4To8((c >> 8) & 0x000f);
		u32 a = Convert4To8((c >> 12) & 0x000f);

		dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
	}
}

void ConvertRGBA5551ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels) {
	for (u32 x = 0; x < numPixels; x++) {
		u16 c = src[x];
		u32 r = Convert5To8(c & 0x001f);
		u32 g = Convert5To8((c >> 5) & 0x001f);
		u32 b = Convert5To8((c >> 10) & 0x001f);
		// We force an arithmetic shift to get the sign bits.
		u32 a = SignExtend16ToU32(c) & 0xff000000;

		dst[x] = a | (r << 16) | (g << 8) | b;
	}
}

void ConvertRGB565ToBGRA8888(u32 *dst, const u16 *src, u32 numPixels) {
	for (u32 x = 0; x < numPixels; x++) {
		u16 c = src[x];
		u32 r = Convert5To8(c & 0x001f);
		u32 g = Convert6To8((c >> 5) & 0x003f);
		u32 b = Convert5To8((c >> 11) & 0x001f);

		dst[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

void ConvertRGBA4444ToABGR4444(u16 *dst, const u16 *src, u32 numPixels) {
	const uint16x8_t mask0040 = vdupq_n_u16(0x00F0);

	if (((uintptr_t)dst & 15) == 0 && ((uintptr_t)src & 15) == 0) {
		u32 simdable = (numPixels / 8) * 8;
		for (u32 i = 0; i < simdable; i += 8) {
			uint16x8_t c = vld1q_u16(src);

			const uint16x8_t a = vshrq_n_u16(c, 12);
			const uint16x8_t b = vandq_u16(vshrq_n_u16(c, 4), mask0040);
			const uint16x8_t g = vshlq_n_u16(vandq_u16(c, mask0040), 4);
			const uint16x8_t r = vshlq_n_u16(c, 12);

			uint16x8_t res = vorrq_u16(vorrq_u16(r, g), vorrq_u16(b, a));
			vst1q_u16(dst, res);

			src += 8;
			dst += 8;
		}
		numPixels -= simdable;
	}
	u32 i = 0;  // already moved the pointers forward

	const u32 *src32 = (const u32 *)src;
	u32 *dst32 = (u32 *)dst;
	for (; i < numPixels / 2; i++) {
		const u32 c = src32[i];
		dst32[i] = ((c >> 12) & 0x000F000F) |
		           ((c >> 4)  & 0x00F000F0) |
		           ((c << 4)  & 0x0F000F00) |
		           ((c << 12) & 0xF000F000);
	}

	if (numPixels & 1) {
		const u32 i = numPixels - 1;
		const u16 c = src[i];
		dst[i] = ((c >> 12) & 0x000F) |
		         ((c >> 4)  & 0x00F0) |
		         ((c << 4)  & 0x0F00) |
		         ((c << 12) & 0xF000);
	}
}

void ConvertRGBA5551ToABGR1555(u16 *dst, const u16 *src, u32 numPixels) {
	const uint16x8_t maskB = vdupq_n_u16(0x003E);
	const uint16x8_t maskG = vdupq_n_u16(0x07C0);

	if (((uintptr_t)dst & 15) == 0 && ((uintptr_t)src & 15) == 0) {
		u32 simdable = (numPixels / 8) * 8;
		for (u32 i = 0; i < simdable; i += 8) {
			uint16x8_t c = vld1q_u16(src);

			const uint16x8_t a = vshrq_n_u16(c, 15);
			const uint16x8_t b = vandq_u16(vshrq_n_u16(c, 9), maskB);
			const uint16x8_t g = vandq_u16(vshlq_n_u16(c, 1), maskG);
			const uint16x8_t r = vshlq_n_u16(c, 11);

			uint16x8_t res = vorrq_u16(vorrq_u16(r, g), vorrq_u16(b, a));
			vst1q_u16(dst, res);

			src += 8;
			dst += 8;
		}
		numPixels -= simdable;
	}
	u32 i = 0;

	const u32 *src32 = (const u32 *)src;
	u32 *dst32 = (u32 *)dst;
	for (; i < numPixels / 2; i++) {
		const u32 c = src32[i];
		dst32[i] = ((c >> 15) & 0x00010001) |
		           ((c >> 9)  & 0x003E003E) |
		           ((c << 1)  & 0x07C007C0) |
		           ((c << 11) & 0xF800F800);
	}

	if (numPixels & 1) {
		const u32 i = numPixels - 1;
		const u16 c = src[i];
		dst[i] = ((c >> 15) & 0x0001) |
		         ((c >> 9)  & 0x003E) |
		         ((c << 1)  & 0x07C0) |
		         ((c << 11) & 0xF800);
	}
}

void ConvertRGB565ToBGR565(u16 *dst, const u16 *src, u32 numPixels) {
	const uint16x8_t maskG = vdupq_n_u16(0x07E0);

	if (((uintptr_t)dst & 15) == 0 && ((uintptr_t)src & 15) == 0) {
		u32 simdable = (numPixels / 8) * 8;
		for (u32 i = 0; i < simdable; i += 8) {
			uint16x8_t c = vld1q_u16(src);

			const uint16x8_t b = vshrq_n_u16(c, 11);
			const uint16x8_t g = vandq_u16(c, maskG);
			const uint16x8_t r = vshlq_n_u16(c, 11);

			uint16x8_t res = vorrq_u16(vorrq_u16(r, g), b);
			vst1q_u16(dst, res);

			src += 8;
			dst += 8;
		}
		numPixels -= simdable;
	}

	u32 i = 0;

	// TODO: Add a 64-bit loop too.
	const u32 *src32 = (const u32 *)src;
	u32 *dst32 = (u32 *)dst;
	for (; i < numPixels / 2; i++) {
		const u32 c = src32[i];
		dst32[i] = ((c >> 11) & 0x001F001F) |
		           ((c >> 0)  & 0x07E007E0) |
		           ((c << 11) & 0xF800F800);
	}

	if (numPixels & 1) {
		const u32 i = numPixels - 1;
		const u16 c = src[i];
		dst[i] = ((c >> 11) & 0x001F) |
		         ((c >> 0)  & 0x07E0) |
		         ((c << 11) & 0xF800);
	}
}

void ConvertBGRA5551ToABGR1555(u16 *dst, const u16 *src, u32 numPixels) {
	const u32 *src32 = (const u32 *)src;
	u32 *dst32 = (u32 *)dst;
	for (u32 i = 0; i < numPixels / 2; i++) {
		const u32 c = src32[i];
		dst32[i] = ((c >> 15) & 0x00010001) | ((c << 1) & 0xFFFEFFFE);
	}

	if (numPixels & 1) {
		const u32 i = numPixels - 1;
		const u16 c = src[i];
		dst[i] = (c >> 15) | (c << 1);
	}
}

static inline u32 premul_pixel_scalar(u32 px) {
	u32 r = (px) & 0xFFu;
	u32 g = (px >> 8) & 0xFFu;
	u32 b = (px >> 16) & 0xFFu;
	u32 a = (px >> 24) & 0xFFu;

	if (a == 255) return px; // already fully opaque
	if (a == 0)  return (a << 24); // transparent (r,g,b = 0)

	// Use (c*a + 128) * 257 >> 16  to approximate (c*a)/255 with good rounding
	u32 ra = ((r * a + 128) * 257) >> 16;
	u32 ga = ((g * a + 128) * 257) >> 16;
	u32 ba = ((b * a + 128) * 257) >> 16;

	return (a << 24) | (ba << 16) | (ga << 8) | ra;
}

void ConvertRGBA8888ToPremulAlpha(u32 *dst, const u32 *src, u32 numPixels) {
	if (!dst || !src || numPixels == 0)
		return;

	u32 i = 0;

	// NEON path (4 pixels per iteration)
	for (; i + 3 < numPixels; i += 4)
	{
		// load 4 pixels as bytes
		uint8x16_t v = vld1q_u8((const uint8_t*)(src + i)); // R0,G0,B0,A0, R1,G1,B1,A1, ...

		// widen to 16-bit lanes
		uint16x8_t lo16 = vmovl_u8(vget_low_u8(v));   // R0,G0,B0,A0, R1,G1,B1,A1
		uint16x8_t hi16 = vmovl_u8(vget_high_u8(v));  // R2,G2,B2,A2, R3,G3,B3,A3

		// read alphas directly from src memory
		const uint8_t* s = (const uint8_t*)src + i * 4;
		const uint16_t a0 = s[3];
		const uint16_t a1 = s[7];
		const uint16_t a2 = s[11];
		const uint16_t a3 = s[15];

		// build alpha vectors (MSVC-friendly, compact)
		uint16x4_t lo = vdup_n_u16(a0);          // R0,G0,B0,A0
		lo = vset_lane_u16(255u, lo, 3);        // A-lane = 256
		uint16x4_t hi = vdup_n_u16(a1);         // R1,G1,B1,A1
		hi = vset_lane_u16(255u, hi, 3);        // A-lane = 256
		uint16x8_t alpha_lo = vcombine_u16(lo, hi);

		lo = vdup_n_u16(a2);                     // R2,G2,B2,A2
		lo = vset_lane_u16(255u, lo, 3);
		hi = vdup_n_u16(a3);                     // R3,G3,B3,A3
		hi = vset_lane_u16(255u, hi, 3);
		uint16x8_t alpha_hi = vcombine_u16(lo, hi);

		// Multiply 16-bit lanes: result fits in 16-bit (truncate shift)
		uint16x8_t prod_lo = vmulq_u16(lo16, alpha_lo);
		uint16x8_t prod_hi = vmulq_u16(hi16, alpha_hi);
		// Apply Jim Blinn's trick.
		prod_lo = vaddq_u16(prod_lo , vdupq_n_u16(128));
		prod_hi = vaddq_u16(prod_hi , vdupq_n_u16(128));
		prod_lo = vaddq_u16(prod_lo , vshrq_n_u16(prod_lo, 8));
		prod_hi = vaddq_u16(prod_hi , vshrq_n_u16(prod_hi, 8));
		uint16x8_t res_lo = vshrq_n_u16(prod_lo, 8);
		uint16x8_t res_hi = vshrq_n_u16(prod_hi, 8);

		// narrow to bytes
		uint8x16_t out = vcombine_u8(vqmovn_u16(res_lo), vqmovn_u16(res_hi));

		// store 4 pixels
		vst1q_u8((uint8_t*)(dst + i), out);
	}

	// Scalar fallback for remaining pixels (or if above SIMD not present)
	for (; i < numPixels; ++i) {
		dst[i] = premul_pixel_scalar(src[i]);
	}
}
