#include "Common/CommonTypes.h"
#include "Common/Data/Convert/ColorConv.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VertexDecoderHandwritten.h"

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

// God of War's T u16 N s8 C 8888 P float: hand-written ARM64 (VertexDecoderHandArm64.cpp: the same operations, two vertices a loop,
// stores in address order). Raw cycles a vertex on the A53 (local/brick/vdecbench/gowbench.cpp, outputs and flag identical),
// destination in the Vulkan memory the emulator decodes into: 14.7 against the compiled C++'s 45.5 (source in L1), 35.9 / 46.1
// (source streamed).
void VtxDec_Tu16_C8888_Pfloat(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	gstate_c.vertexFullAlpha = VdecHand_Tu16pre_N8_C8888_Pf(srcp, dstp, count, uvScaleOffset) != 0;
}

// God of War's skinned W float (1 weight) T u8 C 4444 N s8 P float: hand-written ARM64, two vertices a loop. Raw cycles a vertex
// (local/brick/vdecbench/gowskin.c, against the JIT's own code for it: outputs and flag identical): 38.0 against 70.7 (source in
// L1), 44.6 / 73.7 (streamed). Clears vertexFullAlpha as the JIT's decoders do.
void VtxDec_Wf1_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	if (!VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix))
		gstate_c.vertexFullAlpha = false;
}

// The same with two weights: 43.1 against the JIT's 78.8 (source in L1), 52.2 / 83.7 (streamed); outputs and flag identical.
void VtxDec_Wf2_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	if (!VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix))
		gstate_c.vertexFullAlpha = false;
}

// Tekken 6's skinned W u8 (1-3 weights) T u8 N s8 P s16: hand-written ARM64, two vertices a loop. Raw cycles a vertex
// (local/brick/vdecbench/tkskin.c, against the JIT's own code: outputs identical, with what's written past them), source in L1 /
// in 7-vertex calls (Tekken's average draw): 1x 30.8 / 39.0 against 68.9 / 76.7; 2x 39.4 / 50.1 against 76.5 / 86.4; 3x 48.2 / 63.4
// against 81.2 / 93.2. (Its unskinned T u8 C 565 P s16, VdecHand_Tu8pre_C565_Ps16, takes no bones: 17.9 / 20.8 against 48.1 / 50.3.)
void VtxDec_Wu8x1_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x1_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
}
void VtxDec_Wu8x2_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x2_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
}
void VtxDec_Wu8x3_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x3_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
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
