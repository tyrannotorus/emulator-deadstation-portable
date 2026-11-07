#include "Common/CommonTypes.h"
#include "Common/Data/Convert/ColorConv.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/GPUState.h"

#include "Common/Math/SIMDHeaders.h"

// Candidates for hand-writing
// (found using our custom Very Sleepy).
// GPU::P:_f_N:_s8_C:_8888_T:_u16__(24b)_040001BE  (5%+ of God of War execution)
// GPU::P:_f_N:_s8_C:_8888_T:_u16_W:_f_(1x)__(28b)_040007BE (1%+ of God of War execution)

// Tekken 6:
// (found using the vertex counter that's active in _DEBUG)

// [04000111] P: s16 C: 565 T: u8  (10b) (736949)    // Also in Midnight Club

// Wipeout Pure:
// [0400013f] P: s16 N: s8 C: 8888 T: f  (24b) (1495430)

// Flatout:
// [04000122] P: s16 N: s8 T: u16  (14b) (3901754)
// [04000116] P: s16 C: 5551 T: u16  (12b) (2225841)

// Test drive:
// [05000100] P: s16  (6b) (2827872)
// [050011ff] P: f N: f C: 8888 T: f I: u16  (36b) (3812112)

// Burnout Dominator:
// [04000122] P: s16 N: s8 T: u16  (14b) (1710813)
// [04000116] P: s16 C: 5551 T: u16  (12b) (7688298)

// This is the first GoW one. TODO: Do two verts at a time.
// God of War.
void VtxDec_Tu16_C8888_Pfloat(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	// Input: 4 + 4 + 4 + 12 = 24 bytes
	struct GOWVTX {
		union {
			struct {
				u16 u;
				u16 v;
			};
			u32 packed_uv;
		};
		u32 packed_normal;
		u32 col;
		float x;
		float y;
		float z;
	};
	// NOTE: This might be different for different vertex formats.
	// Output: 8 + 4 + 4 + 12 = 28 bytes.
	struct OutVTX {
		float u;
		float v;
		u32 packed_normal;
		uint32_t col;
		float x;
		float y;
		float z;
	};
	const GOWVTX *src = (const GOWVTX *)srcp;
	OutVTX *dst = (OutVTX *)dstp;
	float uscale = uvScaleOffset->uScale * (1.0f / 32768.0f);
	float vscale = uvScaleOffset->vScale * (1.0f / 32768);
	float uoff = uvScaleOffset->uOff;
	float voff = uvScaleOffset->vOff;

	u32 alpha = 0xFFFFFFFF;

	float32x2_t uvScale = vmul_f32(vld1_f32(&uvScaleOffset->uScale), vdup_n_f32(1.0f / 32768.0f));
	float32x2_t uvOff = vld1_f32(&uvScaleOffset->uOff);
	uint32x4_t alphaMask = vdupq_n_u32(0xFFFFFFFF);
	for (int i = 0; i < count; i++) {
		uint16x4_t uv = vld1_u16(&src[i].u);  // TODO: We only need the first two lanes, maybe there's a better way?
		uint32x2_t fuv = vget_low_u32(vmovl_u16(uv));  // Only using the first two lanes
		float32x2_t finalUV = vadd_f32(vmul_f32(vcvt_f32_u32(fuv), uvScale), uvOff);
		u32 normal = src[i].packed_normal;
		uint32x4_t colpos = vld1q_u32((const u32 *)&src[i].col);
		alphaMask = vandq_u32(alphaMask, colpos);
		vst1_f32(&dst[i].u, finalUV);
		dst[i].packed_normal = normal;
		vst1q_u32(&dst[i].col, colpos);
	}
	alpha = vgetq_lane_u32(alphaMask, 0);
	gstate_c.vertexFullAlpha = (alpha >> 24) == 0xFF;
}

void VtxDec_Tu8_C5551_Ps16(const u8 *srcp, u8 *dstp, int numVerts, const UVScale *uvScaleOffset) {
	struct GTAVTX {
		union {
			struct {
				u8 u;
				u8 v;
			};
			u16 uv;
		};
		u16 col;
		s16 x;
		s16 y;
		s16 z;
	};
	// NOTE: This might be different for different vertex formats.
	struct OutVTX {
		float u;
		float v;
		uint32_t col;
		float x;
		float y;
		float z;
	};
	const GTAVTX *src = (const GTAVTX *)srcp;
	OutVTX *dst = (OutVTX *)dstp;
	float uscale = uvScaleOffset->uScale * (1.0f / 128.0f);
	float vscale = uvScaleOffset->vScale * (1.0f / 128.0f);
	float uoff = uvScaleOffset->uOff;
	float voff = uvScaleOffset->vOff;

	uint64_t alpha = 0xFFFFFFFFFFFFFFFFULL;
	int count = numVerts;


	float32x4_t uvScaleOff = vld1q_f32(&uvScaleOffset->uScale);
	float32x4_t uvScale = vmulq_f32(vcombine_f32(vget_low_f32(uvScaleOff), vget_low_f32(uvScaleOff)), vdupq_n_f32(1.0f / 128.0f));
	float32x4_t uvOffset = vcombine_f32(vget_high_f32(uvScaleOff), vget_high_f32(uvScaleOff));
	float32x4_t posScale = vdupq_n_f32(1.0f / 32768.0f);
	uint32x2_t rmask = vdup_n_u32(0x001F);
	uint32x2_t gmask = vdup_n_u32(0x03E0);
	uint32x2_t bmask = vdup_n_u32(0x7c00);
	uint32x2_t amask = vdup_n_u32(0x8000);
	uint32x2_t lowbits = vdup_n_u32(0x00070707);

	// Two vertices at a time, we can share some calculations.
	// Doing four vertices at a time might be even better, can share more of the pesky color format conversion.
	while (count >= 2) {
		int16x4_t pos0 = vld1_s16(&src[0].x);
		int16x4_t pos1 = vld1_s16(&src[1].x);
		// Translate UV, combined. TODO: Can possibly shuffle UV and col together here
		uint32_t uv0 = (uint32_t)src[0].uv | ((uint32_t)src[1].uv << 16);
		uint64_t col0 = (uint64_t)src[0].col | ((uint64_t)src[1].col << 32);
		int32x4_t pos0_32 = vmovl_s16(pos0);
		int32x4_t pos1_32 = vmovl_s16(pos1);
		float32x4_t pos0_ext = vmulq_f32(vcvtq_f32_s32(pos0_32), posScale);
		float32x4_t pos1_ext = vmulq_f32(vcvtq_f32_s32(pos1_32), posScale);

		uint64x1_t uv8_one = vdup_n_u64(uv0);
		uint8x8_t uv8 = vreinterpret_u8_u64(uv8_one);
		uint16x4_t uv16 = vget_low_u16(vmovl_u8(uv8));
		uint32x4_t uv32 = vmovl_u16(uv16);
		float32x4_t uvf = vaddq_f32(vmulq_f32(vcvtq_f32_u32(uv32), uvScale), uvOffset);

		alpha &= col0;

		// Combined RGBA
		uint32x2_t col = vreinterpret_u32_u64(vdup_n_u64(col0));
		uint32x2_t r = vshl_n_u32(vand_u32(col, rmask), 8 - 5);
		uint32x2_t g = vshl_n_u32(vand_u32(col, gmask), 16 - 10);
		uint32x2_t b = vshl_n_u32(vand_u32(col, bmask), 24 - 15);
		int32x2_t a_shifted = vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(vand_u32(col, amask), 16)), 7);
		uint32x2_t a = vreinterpret_u32_s32(a_shifted);
		col = vorr_u32(vorr_u32(r, g), b);
		col = vorr_u32(col, vand_u32(vshl_n_u32(col, 5), lowbits));
		col = vorr_u32(col, a);

		// TODO: Mix into fewer stores.
		vst1q_f32(&dst[0].x, pos0_ext);
		vst1q_f32(&dst[1].x, pos1_ext);
		vst1_f32(&dst[0].u, vget_low_f32(uvf));
		vst1_f32(&dst[1].u, vget_high_f32(uvf));
		dst[0].col = vget_lane_u32(col, 0);
		dst[1].col = vget_lane_u32(col, 1);

		dst += 2;
		src += 2;
		count -= 2;
	}

	alpha = alpha & (alpha >> 32);


	while (count > 0) {
		float u = src->u * uscale + uoff;
		float v = src->v * vscale + voff;
		alpha &= src->col;
		uint32_t color = RGBA5551ToRGBA8888(src->col);
		float x = src->x * (1.0f / 32768.0f);
		float y = src->y * (1.0f / 32768.0f);
		float z = src->z * (1.0f / 32768.0f);
		dst->col = color;
		dst->u = u;
		dst->v = v;
		dst->x = x;
		dst->y = y;
		dst->z = z;
		src++;
		dst++;
		count--;
	}

	gstate_c.vertexFullAlpha = (alpha >> 15) & 1;
}
