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
// T u8 C 5551 P s16 (upstream's C++ decoder, written for GTA) in ARM64 with the C++'s exact arithmetic (VertexDecoderHandArm64.cpp):
// the same bits, outputs and the alpha result, as the C++ (local/brick/vdecbench/hand5551.c). Cycles a vertex, C++ / JIT / this:
// 7-vertex calls 39.2 / 25.8 / 22.6, from L1 19.5 / 19.9 / 18.9, streamed 20.0 / 20.1 / 19.0. As the C++, it assigns the flag.
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

