#pragma once

// These are useful on JIT-less platforms - they don't beat the jitted vertex decoders by much, but they
// will beat the function-call-stitched ones by a lot.

void VtxDec_Tu16_C8888_Pfloat(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
void VtxDec_Tu8_C5551_Ps16(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);

// Deadstation Portable: hand-written ARM64 (VertexDecoderHandArm64.cpp)
extern "C" void VdecHand_Tu16pre_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
extern "C" void VdecHand_Tu16_Ns8_Pf(const u8 *srcp, u8 *dstp, int count, const UVScale *uvScaleOffset);
