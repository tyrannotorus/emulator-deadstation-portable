#pragma once

// These are useful on JIT-less platforms - they don't beat the jitted vertex decoders by much, but they
// will beat the function-call-stitched ones by a lot.

void VtxDec_Tu16_C8888_Pfloat(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);

// Deadstation Portable: hand-written ARM64 (VertexDecoderHandArm64.cpp)
extern "C" void VdecHand_Tu16pre_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" void VdecHand_Tu16_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" int VdecHand_Tu16pre_N8_C8888_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" int VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset, const float *boneMatrix);
void VtxDec_Wf1_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" int VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset, const float *boneMatrix);
void VtxDec_Wf2_Tu8_C4444_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" void VdecHand_Wu8x1_Tu8pre_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset, const float *boneMatrix);
extern "C" void VdecHand_Wu8x2_Tu8pre_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset, const float *boneMatrix);
extern "C" void VdecHand_Wu8x3_Tu8pre_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset, const float *boneMatrix);
void VtxDec_Wu8x1_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
void VtxDec_Wu8x2_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
void VtxDec_Wu8x3_Tu8_Ns8_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" void VdecHand_Tu8pre_C565_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
