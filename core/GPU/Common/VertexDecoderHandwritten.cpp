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

// T u16 N s8 C 8888 P float: hand-written ARM64 (VertexDecoderHandArm64.cpp), the same operations as the C++ it replaced.
void VtxDec_Tu16_C8888_Pfloat(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	gstate_c.vertexFullAlpha = VdecHand_Tu16pre_N8_C8888_Pf(srcp, dstp, count, uvScaleOffset) != 0;
}

// Skinned W float (1 weight) T u8 C 4444 N s8 P float: hand-written ARM64. Clears vertexFullAlpha as the JIT's decoders do.
void VtxDec_Wf1_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	if (!VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix))
		gstate_c.vertexFullAlpha = false;
}

// The same with two weights.
void VtxDec_Wf2_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	if (!VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix))
		gstate_c.vertexFullAlpha = false;
}

// T u8 C 5551 P s16: upstream's C++ decoder in ARM64 with the C++'s exact arithmetic (VertexDecoderHandArm64.cpp), the same output
// bits and alpha result. As the C++, it assigns the flag.
void VtxDec_Tu8_C5551_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	gstate_c.vertexFullAlpha = VdecHand_Tu8_C5551_Ps16(srcp, dstp, count, uvScaleOffset) != 0;
}

void VtxDec_Wu8x1_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x1_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
}
void VtxDec_Wu8x2_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x2_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
}
void VtxDec_Wu8x3_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset) {
	VdecHand_Wu8x3_Tu8pre_Ns8_Ps16(srcp, dstp, count, uvScaleOffset, gstate.boneMatrix);
}

