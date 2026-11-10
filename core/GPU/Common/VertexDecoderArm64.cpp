// Copyright (c) 2013- PPSSPP Project.

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

#include "Common/CPUDetect.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Common/Arm64Emitter.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VertexDecoderCommon.h"

#include <functional>
#include <vector>

alignas(16) static float bones[16 * 8];  // First four are kept in registers

using namespace Arm64Gen;

// Pointers, X regs (X0 - X17 safe to use.)
static const ARM64Reg srcReg = X0;
static const ARM64Reg dstReg = X1;

static const ARM64Reg counterReg = W2;

static const ARM64Reg uvScaleReg = X3;

static const ARM64Reg tempReg1 = W3;
static const ARM64Reg tempRegPtr = X3;
static const ARM64Reg tempReg2 = W4;
static const ARM64Reg tempReg3 = W5;
static const ARM64Reg scratchReg = W6;
static const ARM64Reg scratchReg64 = X6;
static const ARM64Reg scratchReg2 = W7;
static const ARM64Reg scratchReg3 = W8;
static const ARM64Reg alphaNonFullReg = W12;
static const ARM64Reg boundsMinUReg = W13;
static const ARM64Reg boundsMinVReg = W14;
static const ARM64Reg boundsMaxUReg = W15;
static const ARM64Reg boundsMaxVReg = W16;

static const ARM64Reg fpScratchReg = S4;
static const ARM64Reg fpScratchReg2 = S5;
static const ARM64Reg fpScratchReg3 = S6;
static const ARM64Reg fpScratchReg4 = S7;

static const ARM64Reg neonScratchRegD = D2;
static const ARM64Reg neonScratchRegQ = Q2;
static const ARM64Reg neonScratchReg2D = D3;
static const ARM64Reg neonScratchReg2Q = Q3;

static const ARM64Reg neonUVScaleReg = D0;
static const ARM64Reg neonUVOffsetReg = D1;

static const ARM64Reg src[2] = {S2, S3};
static const ARM64Reg srcD = D2;
static const ARM64Reg srcQ = Q2;

static const ARM64Reg srcNEON = Q8;
static const ARM64Reg accNEON = Q9;

static const ARM64Reg neonWeightRegsQ[2] = { Q3, Q2 };  // reverse order to prevent clash with neonScratchReg in Jit_WeightsU*Skin.

// Q4-Q7 is the generated matrix that we multiply things by.
// Q8,Q9 are accumulators/scratch for matrix mul.
// Q10, Q11 are more scratch for matrix mul.
// Q16+ are free-for-all for matrices. In 16 registers, we can fit 4 4x4 matrices.

static const JitLookup jitLookup[] = {
	{&VertexDecoder::Step_WeightsU8, &VertexDecoderJitCache::Jit_WeightsU8},
	{&VertexDecoder::Step_WeightsU16, &VertexDecoderJitCache::Jit_WeightsU16},
	{&VertexDecoder::Step_WeightsFloat, &VertexDecoderJitCache::Jit_WeightsFloat},
	{&VertexDecoder::Step_WeightsU8Skin, &VertexDecoderJitCache::Jit_WeightsU8Skin},
	{&VertexDecoder::Step_WeightsU16Skin, &VertexDecoderJitCache::Jit_WeightsU16Skin},
	{&VertexDecoder::Step_WeightsFloatSkin, &VertexDecoderJitCache::Jit_WeightsFloatSkin},

	{&VertexDecoder::Step_TcFloat, &VertexDecoderJitCache::Jit_TcFloat},
	{&VertexDecoder::Step_TcU8ToFloat, &VertexDecoderJitCache::Jit_TcU8ToFloat},
	{&VertexDecoder::Step_TcU16ToFloat, &VertexDecoderJitCache::Jit_TcU16ToFloat},

	{&VertexDecoder::Step_TcU8Prescale, &VertexDecoderJitCache::Jit_TcU8Prescale},
	{&VertexDecoder::Step_TcU16Prescale, &VertexDecoderJitCache::Jit_TcU16Prescale},
	{&VertexDecoder::Step_TcFloatPrescale, &VertexDecoderJitCache::Jit_TcFloatPrescale},

	{&VertexDecoder::Step_TcFloatThrough, &VertexDecoderJitCache::Jit_TcFloatThrough},
	{&VertexDecoder::Step_TcU16ThroughToFloat, &VertexDecoderJitCache::Jit_TcU16ThroughToFloat},

	{&VertexDecoder::Step_NormalS8, &VertexDecoderJitCache::Jit_NormalS8},
	{&VertexDecoder::Step_NormalS16, &VertexDecoderJitCache::Jit_NormalS16},
	{&VertexDecoder::Step_NormalFloat, &VertexDecoderJitCache::Jit_NormalFloat},

	{&VertexDecoder::Step_NormalS8Skin, &VertexDecoderJitCache::Jit_NormalS8Skin},
	{&VertexDecoder::Step_NormalS16Skin, &VertexDecoderJitCache::Jit_NormalS16Skin},
	{&VertexDecoder::Step_NormalFloatSkin, &VertexDecoderJitCache::Jit_NormalFloatSkin},

	{&VertexDecoder::Step_NormalS8ToFloat, &VertexDecoderJitCache::Jit_NormalS8ToFloat},  // only used for the D3D11 backend

	{&VertexDecoder::Step_Color8888, &VertexDecoderJitCache::Jit_Color8888},
	{&VertexDecoder::Step_Color4444, &VertexDecoderJitCache::Jit_Color4444},
	{&VertexDecoder::Step_Color565, &VertexDecoderJitCache::Jit_Color565},
	{&VertexDecoder::Step_Color5551, &VertexDecoderJitCache::Jit_Color5551},

	{&VertexDecoder::Step_PosS8Through, &VertexDecoderJitCache::Jit_PosS8Through},
	{&VertexDecoder::Step_PosS16Through, &VertexDecoderJitCache::Jit_PosS16Through},
	{&VertexDecoder::Step_PosFloatThrough, &VertexDecoderJitCache::Jit_PosFloatThrough},

	{&VertexDecoder::Step_PosS8, &VertexDecoderJitCache::Jit_PosS8},
	{&VertexDecoder::Step_PosS16, &VertexDecoderJitCache::Jit_PosS16},
	{&VertexDecoder::Step_PosFloat, &VertexDecoderJitCache::Jit_PosFloat},

	{&VertexDecoder::Step_PosS8Skin, &VertexDecoderJitCache::Jit_PosS8Skin},
	{&VertexDecoder::Step_PosS16Skin, &VertexDecoderJitCache::Jit_PosS16Skin},
	{&VertexDecoder::Step_PosFloatSkin, &VertexDecoderJitCache::Jit_PosFloatSkin},

	/*
	{&VertexDecoder::Step_NormalS8Morph, &VertexDecoderJitCache::Jit_NormalS8Morph},
	{&VertexDecoder::Step_NormalS16Morph, &VertexDecoderJitCache::Jit_NormalS16Morph},
	{&VertexDecoder::Step_NormalFloatMorph, &VertexDecoderJitCache::Jit_NormalFloatMorph},

	{&VertexDecoder::Step_PosS8Morph, &VertexDecoderJitCache::Jit_PosS8Morph},
	{&VertexDecoder::Step_PosS16Morph, &VertexDecoderJitCache::Jit_PosS16Morph},
	{&VertexDecoder::Step_PosFloatMorph, &VertexDecoderJitCache::Jit_PosFloatMorph},

	{&VertexDecoder::Step_Color8888Morph, &VertexDecoderJitCache::Jit_Color8888Morph},
	{&VertexDecoder::Step_Color4444Morph, &VertexDecoderJitCache::Jit_Color4444Morph},
	{&VertexDecoder::Step_Color565Morph, &VertexDecoderJitCache::Jit_Color565Morph},
	{&VertexDecoder::Step_Color5551Morph, &VertexDecoderJitCache::Jit_Color5551Morph},
	*/
};


JittedVertexDecoder VertexDecoderJitCache::Compile(const VertexDecoder &dec, int32_t *jittedSize) {
	dec_ = &dec;

	BeginWrite(4096);
	const u8 *start = AlignCode16();

	bool prescaleStep = false;
	bool skinning = false;
	bool updateTexBounds = false;

	bool log = false;

	// Look for prescaled texcoord steps
	for (int i = 0; i < dec.numSteps_; i++) {
		if (dec.steps_[i] == &VertexDecoder::Step_TcU8Prescale ||
			dec.steps_[i] == &VertexDecoder::Step_TcU16Prescale ||
			dec.steps_[i] == &VertexDecoder::Step_TcFloatPrescale) {
			prescaleStep = true;
		}
		if (dec.steps_[i] == &VertexDecoder::Step_WeightsU8Skin ||
			dec.steps_[i] == &VertexDecoder::Step_WeightsU16Skin ||
			dec.steps_[i] == &VertexDecoder::Step_WeightsFloatSkin) {
			skinning = true;
		}
		if (dec.steps_[i] == &VertexDecoder::Step_TcU16ThroughToFloat) {
			updateTexBounds = true;
		}
	}

	// Not used below, but useful for logging.
	(void)skinning;

	// if (skinning) log = true;

	bool updateFullAlpha = dec.col;
	if (updateFullAlpha && (dec.VertexType() & GE_VTYPE_COL_MASK) == GE_VTYPE_COL_565)
		updateFullAlpha = false;

	// Deadstation Portable: two vertices a loop (Jit_Pair, Jit_PairSkinned); skinned, it reads bones 2 up from the array; with
	// texture bounds tracked (W13-W16), its second vertex uses X19-X22, saved.
	const bool pair = PairSupported(dec);
	// GPRs 0-15 do not need to be saved.
	// We don't use any higher GPRs than 16. So:
	uint64_t regs_to_save = updateTexBounds ? 1 << 16 : 0;
	if (pair && updateTexBounds)
		regs_to_save |= (1 << 19) | (1 << 20) | (1 << 21) | (1 << 22);
	// We only need to save Q8-Q15 if skinning is used.
	uint64_t regs_to_save_fp = dec.skinInDecode ? Arm64Gen::ALL_CALLEE_SAVED_FP : 0;
	// Only bother making stack space and setting up FP if there are saved regs.
	if (regs_to_save || regs_to_save_fp)
		fp.ABI_PushRegisters(regs_to_save, regs_to_save_fp);

	// Keep the scale/offset in a few fp registers if we need it.
	if (prescaleStep) {
		fp.LDP(64, INDEX_SIGNED, neonUVScaleReg, neonUVOffsetReg, X3, 0);
	}

	// Add code to convert matrices to 4x4.
	// Later we might want to do this when the matrices are loaded instead.
	const int firstStoredBone = pair && dec.skinInDecode ? 2 : 4;
	if (dec.skinInDecode) {
		// Copying from R3 to R4
		MOVP2R(X3, gstate.boneMatrix);
		// This is only used with more than 4 weights, and points to the first of them.
		if (dec.nweights > firstStoredBone)
			MOVP2R(X4, &bones[16 * firstStoredBone]);

		// Construct a mask to zero out the top lane with.
		fp.MVNI(32, Q3, 0);
		fp.MOVI(32, Q4, 0);
		fp.EXT(Q3, Q3, Q4, 4);

		for (int i = 0; i < dec.nweights; i++) {
			// This loads Q4,Q5,Q6 with 12 floats and increases X3, all in one go.
			fp.LD1(32, 3, INDEX_POST, Q4, X3);
			// Now sort those floats into 4 regs: ABCD EFGH IJKL -> ABC0 DEF0 GHI0 JKL0.
			// Go backwards to avoid overwriting.
			fp.EXT(Q7, Q6, Q6, 4); // I[JKLI]JKL
			fp.EXT(Q6, Q5, Q6, 8); // EF[GHIJ]KL
			fp.EXT(Q5, Q4, Q5, 12); // ABC[DEFG]H

			ARM64Reg matrixRow[4]{ Q4, Q5, Q6, Q7 };
			// First four matrices are in registers Q16+.
			if (i < 4) {
				for (int w = 0; w < 4; ++w)
					matrixRow[w] = (ARM64Reg)(Q16 + i * 4 + w);
			}
			// Zero out the top lane of each one with the mask created above.
			fp.AND(matrixRow[0], Q4, Q3);
			fp.AND(matrixRow[1], Q5, Q3);
			fp.AND(matrixRow[2], Q6, Q3);
			fp.AND(matrixRow[3], Q7, Q3);

			if (i >= firstStoredBone)
				fp.ST1(32, 4, INDEX_POST, matrixRow[0], X4);
		}
	}

	if (updateFullAlpha) {
		// This ends up non-zero if alpha is not full.
		// Often we just ORN into it.
		MOVI2R(alphaNonFullReg, 0);
	}

	if (updateTexBounds) {
		MOVP2R(scratchReg64, &gstate_c.vertBounds.minU);
		LDRH(INDEX_UNSIGNED, boundsMinUReg, scratchReg64, offsetof(KnownVertexBounds, minU));
		LDRH(INDEX_UNSIGNED, boundsMaxUReg, scratchReg64, offsetof(KnownVertexBounds, maxU));
		LDRH(INDEX_UNSIGNED, boundsMinVReg, scratchReg64, offsetof(KnownVertexBounds, minV));
		LDRH(INDEX_UNSIGNED, boundsMaxVReg, scratchReg64, offsetof(KnownVertexBounds, maxV));
	}

	// Deadstation Portable: two vertices a loop first (Jit_Pair, Jit_PairSkinned), this one-vertex loop after it for an odd last one.
	FixupBranch pairsDone;
	if (pair) {
		if (prescaleStep) {
			// v's scale and offset as scalars of their own (u's are S0 and S1, the low lanes of D0 and D1): Q16/Q17 unskinned,
			// Q2/Q3 skinned (Q16 up hold bones there)
			fp.INS(32, dec.skinInDecode ? Q2 : Q16, 0, Q0, 1);
			fp.INS(32, dec.skinInDecode ? Q3 : Q17, 0, Q1, 1);
		}
		if (dec.skinInDecode && dec.nweights > 2)
			MOVP2R(X8, &bones[16 * 2]);
		SUBS(counterReg, counterReg, 2);
		FixupBranch noPairs = B(CC_LT);
		const u8 *pairStart = NopAlignCode16();
		if (dec.skinInDecode)
			Jit_PairSkinned(dec);
		else
			Jit_Pair(dec);
		ADDI2R(srcReg, srcReg, dec.VertexSize() * 2, scratchReg);
		ADDI2R(dstReg, dstReg, dec.decFmt.stride * 2, scratchReg);
		SUBS(counterReg, counterReg, 2);
		B(CC_GE, pairStart);
		SetJumpTarget(noPairs);
		ADDI2R(counterReg, counterReg, 2, scratchReg);
		pairsDone = CBZ(counterReg);
		// the one-vertex loop keeps bones 2 and 3 in Q24-Q31, which the pair loop used
		if (dec.skinInDecode && dec.nweights > 2) {
			MOVP2R(X4, &bones[16 * 2]);
			fp.LD1(32, 4, INDEX_POST, Q24, X4);
			if (dec.nweights > 3)
				fp.LD1(32, 4, INDEX_POST, Q28, X4);
		}
	}

	const u8 *loopStart = NopAlignCode16();
	// Deadstation Portable: a skinned vertex's normal and position loaded before anything else, into registers no step touches
	// (Q12, Q13), so the loads complete while the weights blend the matrix; the position step then transforms both, interleaved
	// (each was loaded and used at once before: the two stalls 21% of God of War's skinned decoder).
	skinPreload_ = dec.skinInDecode;
	skinNormalPending_ = false;
	if (skinPreload_) {
		for (int i = 0; i < dec.numSteps_; i++) {
			const auto st = dec.steps_[i];
			if (st == &VertexDecoder::Step_NormalS8Skin)
				fp.LDUR(32, S12, srcReg, dec.nrmoff);
			else if (st == &VertexDecoder::Step_NormalS16Skin)
				fp.LDUR(64, D12, srcReg, dec.nrmoff);
			else if (st == &VertexDecoder::Step_NormalFloatSkin)
				fp.LDUR(128, Q12, srcReg, dec.nrmoff);
			else if (st == &VertexDecoder::Step_PosS8Skin)
				fp.LDUR(32, S13, srcReg, dec.posoff);
			else if (st == &VertexDecoder::Step_PosS16Skin)
				fp.LDUR(64, D13, srcReg, dec.posoff);
			else if (st == &VertexDecoder::Step_PosFloatSkin)
				fp.LDUR(128, Q13, srcReg, dec.posoff);
		}
	}
	for (int i = 0; i < dec.numSteps_; i++) {
		if (!CompileStep(dec, i)) {
			EndWrite();
			// Reset the code ptr (effectively undoing what we generated) and return zero to indicate that we failed.
			ResetCodePtr(GetOffset(start));
			char temp[1024]{};
			dec.ToString(temp, true);
			WARN_LOG(Log::G3D, "Could not compile vertex decoder, failed at step %s: %s", GetStepFunctionName(dec.steps_[i]), temp);
			return nullptr;
		}
	}

	ADDI2R(srcReg, srcReg, dec.VertexSize(), scratchReg);
	ADDI2R(dstReg, dstReg, dec.decFmt.stride, scratchReg);
	SUBS(counterReg, counterReg, 1);
	B(CC_NEQ, loopStart);
	if (pair)
		SetJumpTarget(pairsDone);

	if (updateFullAlpha) {
		FixupBranch skip = CBZ(alphaNonFullReg);
		MOVP2R(tempRegPtr, &gstate_c.vertexFullAlpha);
		STRB(INDEX_UNSIGNED, WZR, tempRegPtr, 0);
		SetJumpTarget(skip);
	}

	if (updateTexBounds) {
		MOVP2R(scratchReg64, &gstate_c.vertBounds.minU);
		STRH(INDEX_UNSIGNED, boundsMinUReg, scratchReg64, offsetof(KnownVertexBounds, minU));
		STRH(INDEX_UNSIGNED, boundsMaxUReg, scratchReg64, offsetof(KnownVertexBounds, maxU));
		STRH(INDEX_UNSIGNED, boundsMinVReg, scratchReg64, offsetof(KnownVertexBounds, minV));
		STRH(INDEX_UNSIGNED, boundsMaxVReg, scratchReg64, offsetof(KnownVertexBounds, maxV));
	}

	if (regs_to_save || regs_to_save_fp)
		fp.ABI_PopRegisters(regs_to_save, regs_to_save_fp);

	RET();

	FlushIcache();

	*jittedSize = (int)(GetCodePtr() - start);
	EndWrite();
	return (JittedVertexDecoder)start;
}

bool VertexDecoderJitCache::CompileStep(const VertexDecoder &dec, int step) {
	// See if we find a matching JIT function
	for (size_t i = 0; i < ARRAY_SIZE(jitLookup); i++) {
		if (dec.steps_[step] == jitLookup[i].func) {
			((*this).*jitLookup[i].jitFunc)();
			return true;
		}
	}
	return false;
}

void VertexDecoderJitCache::Jit_ApplyWeights() {
	// We construct a matrix in Q4-Q7
	if (dec_->nweights > 4) {
		MOVP2R(scratchReg64, bones + 16 * 4);
	}
	for (int i = 0; i < dec_->nweights; i++) {
		switch (i) {
		case 0:
			fp.FMUL(32, Q4, Q16, neonWeightRegsQ[0], 0);
			fp.FMUL(32, Q5, Q17, neonWeightRegsQ[0], 0);
			fp.FMUL(32, Q6, Q18, neonWeightRegsQ[0], 0);
			fp.FMUL(32, Q7, Q19, neonWeightRegsQ[0], 0);
			break;
		case 1:
			fp.FMLA(32, Q4, Q20, neonWeightRegsQ[0], 1);
			fp.FMLA(32, Q5, Q21, neonWeightRegsQ[0], 1);
			fp.FMLA(32, Q6, Q22, neonWeightRegsQ[0], 1);
			fp.FMLA(32, Q7, Q23, neonWeightRegsQ[0], 1);
			break;
		case 2:
			fp.FMLA(32, Q4, Q24, neonWeightRegsQ[0], 2);
			fp.FMLA(32, Q5, Q25, neonWeightRegsQ[0], 2);
			fp.FMLA(32, Q6, Q26, neonWeightRegsQ[0], 2);
			fp.FMLA(32, Q7, Q27, neonWeightRegsQ[0], 2);
			break;
		case 3:
			fp.FMLA(32, Q4, Q28, neonWeightRegsQ[0], 3);
			fp.FMLA(32, Q5, Q29, neonWeightRegsQ[0], 3);
			fp.FMLA(32, Q6, Q30, neonWeightRegsQ[0], 3);
			fp.FMLA(32, Q7, Q31, neonWeightRegsQ[0], 3);
			break;
		default:
			// Matrices 4+ need to be loaded from memory.
			fp.LD1(32, 4, INDEX_POST, Q8, scratchReg64);
			fp.FMLA(32, Q4, Q8, neonWeightRegsQ[i >> 2], i & 3);
			fp.FMLA(32, Q5, Q9, neonWeightRegsQ[i >> 2], i & 3);
			fp.FMLA(32, Q6, Q10, neonWeightRegsQ[i >> 2], i & 3);
			fp.FMLA(32, Q7, Q11, neonWeightRegsQ[i >> 2], i & 3);
			break;
		}
	}
}

void VertexDecoderJitCache::Jit_WeightsU8() {
	// Basic implementation - a byte at a time. TODO: Optimize
	int j;
	for (j = 0; j < dec_->nweights; j++) {
		LDRB(INDEX_UNSIGNED, tempReg1, srcReg, dec_->weightoff + j);
		STRB(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.w0off + j);
	}
	while (j & 3) {
		STRB(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.w0off + j);
		j++;
	}
}

void VertexDecoderJitCache::Jit_WeightsU16() {
	// Basic implementation - a short at a time. TODO: Optimize
	int j;
	for (j = 0; j < dec_->nweights; j++) {
		LDRH(INDEX_UNSIGNED, tempReg1, srcReg, dec_->weightoff + j * 2);
		STRH(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.w0off + j * 2);
	}
	while (j & 3) {
		STRH(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.w0off + j * 2);
		j++;
	}
}

void VertexDecoderJitCache::Jit_WeightsFloat() {
	int j;
	for (j = 0; j < dec_->nweights; j++) {
		LDR(INDEX_UNSIGNED, tempReg1, srcReg, dec_->weightoff + j * 4);
		STR(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.w0off + j * 4);
	}
	while (j & 3) {  // Zero additional weights rounding up to 4.
		STR(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.w0off + j * 4);
		j++;
	}
}

void VertexDecoderJitCache::Jit_WeightsU8Skin() {
	// Weight is first so srcReg is correct.
	switch (dec_->nweights) {
	case 1: fp.LDR(8, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0); break;
	case 2: fp.LDR(16, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0); break;
	default:
		// For 3, we over read, for over 4, we read more later.
		fp.LDR(32, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0);
		break;
	}

	fp.UXTL(8, neonScratchRegQ, neonScratchRegD);
	fp.UXTL(16, neonScratchRegQ, neonScratchRegD);
	fp.UCVTF(32, neonWeightRegsQ[0], neonScratchRegQ, 7);

	if (dec_->nweights > 4) {
		switch (dec_->nweights) {
		case 5: fp.LDR(8, INDEX_UNSIGNED, neonScratchRegD, srcReg, 4); break;
		case 6: fp.LDR(16, INDEX_UNSIGNED, neonScratchRegD, srcReg, 4); break;
		case 7:
		case 8:
			fp.LDR(32, INDEX_UNSIGNED, neonScratchRegD, srcReg, 4);
			break;
		}
		fp.UXTL(8, neonScratchRegQ, neonScratchRegD);
		fp.UXTL(16, neonScratchRegQ, neonScratchRegD);
		fp.UCVTF(32, neonWeightRegsQ[1], neonScratchRegQ, 7);
	}
	Jit_ApplyWeights();
}

void VertexDecoderJitCache::Jit_WeightsU16Skin() {
	switch (dec_->nweights) {
	case 1: fp.LDR(16, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0); break;
	case 2: fp.LDR(32, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0); break;
	default:
		// For 3, we over read, for over 4, we read more later.
		fp.LDR(64, INDEX_UNSIGNED, neonScratchRegD, srcReg, 0);
		break;
	}
	fp.UXTL(16, neonScratchRegQ, neonScratchRegD);
	fp.UCVTF(32, neonWeightRegsQ[0], neonScratchRegQ, 15);

	if (dec_->nweights > 4) {
		switch (dec_->nweights) {
		case 5: fp.LDR(16, INDEX_UNSIGNED, neonScratchRegD, srcReg, 8); break;
		case 6: fp.LDR(32, INDEX_UNSIGNED, neonScratchRegD, srcReg, 8); break;
		case 7:
		case 8:
			fp.LDR(64, INDEX_UNSIGNED, neonScratchRegD, srcReg, 8);
			break;
		}
		fp.UXTL(16, neonScratchRegQ, neonScratchRegD);
		fp.UCVTF(32, neonWeightRegsQ[1], neonScratchRegQ, 15);
	}
	Jit_ApplyWeights();
}

void VertexDecoderJitCache::Jit_WeightsFloatSkin() {
	switch (dec_->nweights) {
	case 1:
		fp.LDR(32, INDEX_UNSIGNED, neonWeightRegsQ[0], srcReg, 0);
		break;
	case 2:
		fp.LDR(64, INDEX_UNSIGNED, neonWeightRegsQ[0], srcReg, 0);
		break;
	case 3:
	case 4:
		fp.LDR(128, INDEX_UNSIGNED, neonWeightRegsQ[0], srcReg, 0);
		break;

	case 5:
		fp.LDR(128, INDEX_UNSIGNED, neonWeightRegsQ[0], srcReg, 0);
		fp.LDR(32, INDEX_UNSIGNED, neonWeightRegsQ[1], srcReg, 16);
		break;
	case 6:
		fp.LDR(128, INDEX_UNSIGNED, neonWeightRegsQ[0], srcReg, 0);
		fp.LDR(64, INDEX_UNSIGNED, neonWeightRegsQ[1], srcReg, 16);
		break;
	case 7:
	case 8:
		fp.LDP(128, INDEX_SIGNED, neonWeightRegsQ[0], neonWeightRegsQ[1], srcReg, 0);
		break;
	}
	Jit_ApplyWeights();
}

void VertexDecoderJitCache::Jit_Color8888() {
	LDR(INDEX_UNSIGNED, tempReg1, srcReg, dec_->coloff);

	// Or any non-set bits into alphaNonFullReg.  This way it's non-zero if not full.
	ORN(alphaNonFullReg, alphaNonFullReg, tempReg1, ArithOption(tempReg1, ST_ASR, 24));

	STR(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.c0off);
}

void VertexDecoderJitCache::Jit_Color4444() {
	LDRH(INDEX_UNSIGNED, tempReg1, srcReg, dec_->coloff);

	// Spread out the components.
	ANDI2R(tempReg2, tempReg1, 0x000F, scratchReg);
	ANDI2R(tempReg3, tempReg1, 0x00F0, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 4));
	ANDI2R(tempReg3, tempReg1, 0x0F00, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 8));
	ANDI2R(tempReg3, tempReg1, 0xF000, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 12));

	// And expand to 8 bits.
	ORR(tempReg1, tempReg2, tempReg2, ArithOption(tempReg2, ST_LSL, 4));

	// Or any non-set bits into alphaNonFullReg.  This way it's non-zero if not full.
	ORN(alphaNonFullReg, alphaNonFullReg, tempReg1, ArithOption(tempReg1, ST_ASR, 24));

	STR(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.c0off);
}

void VertexDecoderJitCache::Jit_Color565() {
	LDRH(INDEX_UNSIGNED, tempReg1, srcReg, dec_->coloff);

	// Spread out R and B first.  This puts them in 0x001F001F.
	ANDI2R(tempReg2, tempReg1, 0x001F, scratchReg);
	ANDI2R(tempReg3, tempReg1, 0xF800, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 5));

	// Expand 5 -> 8.
	LSL(tempReg3, tempReg2, 3);
	ORR(tempReg2, tempReg3, tempReg2, ArithOption(tempReg2, ST_LSR, 2));
	ANDI2R(tempReg2, tempReg2, 0xFFFF00FF, scratchReg);

	// Now finally G.  We start by shoving it into a wall.
	LSR(tempReg1, tempReg1, 5);
	ANDI2R(tempReg1, tempReg1, 0x003F, scratchReg);
	LSL(tempReg3, tempReg1, 2);
	// Don't worry, shifts into a wall.
	ORR(tempReg3, tempReg3, tempReg1, ArithOption(tempReg1, ST_LSR, 4));
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 8));

	// Add in full alpha.  No need to update alphaNonFullReg.
	ORRI2R(tempReg1, tempReg2, 0xFF000000, scratchReg);

	STR(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.c0off);
}

void VertexDecoderJitCache::Jit_Color5551() {
	LDRSH(INDEX_UNSIGNED, tempReg1, srcReg, dec_->coloff);

	ANDI2R(tempReg2, tempReg1, 0x001F, scratchReg);
	ANDI2R(tempReg3, tempReg1, 0x03E0, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 3));
	ANDI2R(tempReg3, tempReg1, 0x7C00, scratchReg);
	ORR(tempReg2, tempReg2, tempReg3, ArithOption(tempReg3, ST_LSL, 6));

	// Expand 5 -> 8.
	LSR(tempReg3, tempReg2, 2);
	// Clean up the bits that were shifted right.
	ANDI2R(tempReg3, tempReg3, ~0x000000F8);
	ANDI2R(tempReg3, tempReg3, ~0x0000F800);
	ORR(tempReg2, tempReg3, tempReg2, ArithOption(tempReg2, ST_LSL, 3));

	// Now we just need alpha.  Since we loaded as signed, it'll be extended.
	ANDI2R(tempReg1, tempReg1, 0xFF000000, scratchReg);
	ORR(tempReg2, tempReg2, tempReg1);
	
	// Or any non-set bits into alphaNonFullReg.  This way it's non-zero if not full.
	ORN(alphaNonFullReg, alphaNonFullReg, tempReg1, ArithOption(tempReg1, ST_ASR, 24));

	STR(INDEX_UNSIGNED, tempReg2, dstReg, dec_->decFmt.c0off);
}

void VertexDecoderJitCache::Jit_TcU16ThroughToFloat() {
	LDRH(INDEX_UNSIGNED, tempReg1, srcReg, dec_->tcoff);
	LDRH(INDEX_UNSIGNED, tempReg2, srcReg, dec_->tcoff + 2);

	auto updateSide = [&](ARM64Reg src, CCFlags cc, ARM64Reg dst) {
		CMP(src, dst);
		CSEL(dst, src, dst, cc);
	};

	updateSide(tempReg1, CC_LT, boundsMinUReg);
	updateSide(tempReg1, CC_GT, boundsMaxUReg);
	updateSide(tempReg2, CC_LT, boundsMinVReg);
	updateSide(tempReg2, CC_GT, boundsMaxVReg);

	fp.LDUR(32, neonScratchRegD, srcReg, dec_->tcoff);
	fp.UXTL(16, neonScratchRegQ, neonScratchRegD); // Widen to 32-bit
	fp.UCVTF(32, neonScratchRegD, neonScratchRegD);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcFloatThrough() {
	LDP(INDEX_SIGNED, tempReg1, tempReg2, srcReg, dec_->tcoff);
	STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcFloat() {
	LDP(INDEX_SIGNED, tempReg1, tempReg2, srcReg, dec_->tcoff);
	STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcU8Prescale() {
	fp.LDUR(16, neonScratchReg2D, srcReg, dec_->tcoff);
	fp.UXTL(8, neonScratchReg2Q, neonScratchReg2D); // Widen to 16-bit
	fp.UXTL(16, neonScratchReg2Q, neonScratchReg2D); // Widen to 32-bit
	fp.UCVTF(32, neonScratchReg2D, neonScratchReg2D, 7);
	fp.MOV(neonScratchRegD, neonUVOffsetReg);
	fp.FMLA(32, neonScratchRegD, neonScratchReg2D, neonUVScaleReg);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcU8ToFloat() {
	fp.LDUR(16, neonScratchRegD, srcReg, dec_->tcoff);
	fp.UXTL(8, neonScratchRegQ, neonScratchRegD); // Widen to 16-bit
	fp.UXTL(16, neonScratchRegQ, neonScratchRegD); // Widen to 32-bit
	fp.UCVTF(32, neonScratchRegD, neonScratchRegD, 7);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcU16Prescale() {
	fp.LDUR(32, neonScratchReg2D, srcReg, dec_->tcoff);
	fp.UXTL(16, neonScratchReg2Q, neonScratchReg2D); // Widen to 32-bit
	fp.UCVTF(32, neonScratchReg2D, neonScratchReg2D, 15);
	fp.MOV(neonScratchRegD, neonUVOffsetReg);
	fp.FMLA(32, neonScratchRegD, neonScratchReg2D, neonUVScaleReg);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcU16ToFloat() {
	fp.LDUR(32, neonScratchRegD, srcReg, dec_->tcoff);
	fp.UXTL(16, neonScratchRegQ, neonScratchRegD); // Widen to 32-bit
	fp.UCVTF(32, neonScratchRegD, neonScratchRegD, 15);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_TcFloatPrescale() {
	fp.LDUR(64, neonScratchReg2D, srcReg, dec_->tcoff);
	fp.MOV(neonScratchRegD, neonUVOffsetReg);
	fp.FMLA(32, neonScratchRegD, neonScratchReg2D, neonUVScaleReg);
	fp.STUR(64, neonScratchRegD, dstReg, dec_->decFmt.uvoff);
}

void VertexDecoderJitCache::Jit_PosS8() {
	Jit_AnyS8ToFloat(dec_->posoff);
	fp.STUR(128, srcQ, dstReg, dec_->decFmt.posoff);
}

void VertexDecoderJitCache::Jit_PosS16() {
	Jit_AnyS16ToFloat(dec_->posoff);
	fp.STUR(128, srcQ, dstReg, dec_->decFmt.posoff);
}

void VertexDecoderJitCache::Jit_PosFloat() {
	// Only need to copy 12 bytes, but copying 16 should be okay (and is faster.)
	if ((dec_->posoff & 7) == 0 && (dec_->decFmt.posoff & 7) == 0) {
		LDP(INDEX_SIGNED, EncodeRegTo64(tempReg1), EncodeRegTo64(tempReg2), srcReg, dec_->posoff);
		STP(INDEX_SIGNED, EncodeRegTo64(tempReg1), EncodeRegTo64(tempReg2), dstReg, dec_->decFmt.posoff);
	} else {
		LDP(INDEX_SIGNED, tempReg1, tempReg2, srcReg, dec_->posoff);
		STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.posoff);
		LDR(INDEX_UNSIGNED, tempReg3, srcReg, dec_->posoff + 8);
		STR(INDEX_UNSIGNED, tempReg3, dstReg, dec_->decFmt.posoff + 8);
	}
}

void VertexDecoderJitCache::Jit_PosS8Through() {
	// 8-bit positions in throughmode always decode to 0, depth included.
	STR(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.posoff);
	STR(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.posoff + 4);
	STR(INDEX_UNSIGNED, WZR, dstReg, dec_->decFmt.posoff + 8);
}

void VertexDecoderJitCache::Jit_PosS16Through() {
	// Start with X and Y (which is signed.)
	fp.LDUR(32, src[0], srcReg, dec_->posoff);
	fp.SXTL(16, srcD, src[0]);
	fp.SCVTF(32, srcD, srcD);
	fp.STUR(64, src[0], dstReg, dec_->decFmt.posoff);
	// Now load in Z (which is unsigned.)
	LDRH(INDEX_UNSIGNED, tempReg3, srcReg, dec_->posoff + 4);
	fp.SCVTF(src[1], tempReg3);
	STR(INDEX_UNSIGNED, src[1], dstReg, dec_->decFmt.posoff + 8);
}

void VertexDecoderJitCache::Jit_PosFloatThrough() {
	// Instead of just copying 12 bytes, we copy 8 and clamp Z.
	if ((dec_->posoff & 7) == 0 && (dec_->decFmt.posoff & 7) == 0) {
		LDR(INDEX_UNSIGNED, EncodeRegTo64(tempReg1), srcReg, dec_->posoff);
		STR(INDEX_UNSIGNED, EncodeRegTo64(tempReg1), dstReg, dec_->decFmt.posoff);
	} else {
		LDP(INDEX_SIGNED, tempReg1, tempReg2, srcReg, dec_->posoff);
		STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.posoff);
	}

	fp.LDUR(32, neonScratchRegD, srcReg, dec_->posoff + 8);
	fp.FCVTZU(32, neonScratchRegD, neonScratchRegD);
	// Narrow to 16 bit, saturating meanwhile.
	fp.UQXTN(16, neonScratchRegD, neonScratchRegD);
	fp.UXTL(16, neonScratchRegD, neonScratchRegD);
	fp.UCVTF(32, neonScratchRegD, neonScratchRegD);
	fp.STUR(32, neonScratchRegD, dstReg, dec_->decFmt.posoff + 8);
}

void VertexDecoderJitCache::Jit_NormalS8() {
	LDURH(tempReg1, srcReg, dec_->nrmoff);
	LDRB(INDEX_UNSIGNED, tempReg3, srcReg, dec_->nrmoff + 2);
	ORR(tempReg1, tempReg1, tempReg3, ArithOption(tempReg3, ST_LSL, 16));
	STR(INDEX_UNSIGNED, tempReg1, dstReg, dec_->decFmt.nrmoff);
}

// Copy 6 bytes and then 2 zeroes.
void VertexDecoderJitCache::Jit_NormalS16() {
	// NOTE: Not LDRH, we just copy the raw bytes here.
	LDUR(tempReg1, srcReg, dec_->nrmoff);
	LDRH(INDEX_UNSIGNED, tempReg2, srcReg, dec_->nrmoff + 4);
	STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.nrmoff);
}

void VertexDecoderJitCache::Jit_NormalFloat() {
	// Only need to copy 12 bytes, but copying 16 should be okay (and is faster.)
	if ((dec_->nrmoff & 7) == 0 && (dec_->decFmt.nrmoff & 7) == 0) {
		LDP(INDEX_SIGNED, EncodeRegTo64(tempReg1), EncodeRegTo64(tempReg2), srcReg, dec_->nrmoff);
		STP(INDEX_SIGNED, EncodeRegTo64(tempReg1), EncodeRegTo64(tempReg2), dstReg, dec_->decFmt.nrmoff);
	} else {
		LDP(INDEX_SIGNED, tempReg1, tempReg2, srcReg, dec_->nrmoff);
		STP(INDEX_SIGNED, tempReg1, tempReg2, dstReg, dec_->decFmt.nrmoff);
		LDR(INDEX_UNSIGNED, tempReg3, srcReg, dec_->nrmoff + 8);
		STR(INDEX_UNSIGNED, tempReg3, dstReg, dec_->decFmt.nrmoff + 8);
	}
}

void VertexDecoderJitCache::Jit_NormalS8ToFloat() {
	Jit_AnyS8ToFloat(dec_->nrmoff);
	fp.STUR(128, srcQ, dstReg, dec_->decFmt.nrmoff);
}

// Skinned: the normal (preloaded in Q12) converted here, transformed with the position in Jit_SkinTransform.
void VertexDecoderJitCache::Jit_NormalS8Skin() {
	fp.SXTL(8, D14, S12);
	fp.SXTL(16, Q14, D14);
	fp.SCVTF(32, Q14, Q14, 7);
	skinNormalReg_ = Q14;
	skinNormalOff_ = dec_->decFmt.nrmoff;
	skinNormalPending_ = true;
}

void VertexDecoderJitCache::Jit_NormalS16Skin() {
	fp.SXTL(16, Q14, D12);
	fp.SCVTF(32, Q14, Q14, 15);
	skinNormalReg_ = Q14;
	skinNormalOff_ = dec_->decFmt.nrmoff;
	skinNormalPending_ = true;
}

void VertexDecoderJitCache::Jit_NormalFloatSkin() {
	skinNormalReg_ = Q12;
	skinNormalOff_ = dec_->decFmt.nrmoff;
	skinNormalPending_ = true;
}

// Skinned: the position (preloaded in Q13) converted, then the position and the normal transformed by the blended matrix (Q4-Q7)
// with an accumulator each (Q15, Q9), their multiplies interleaved (each accumulator's operations in the same order as before:
// the same bits), stored in address order.
void VertexDecoderJitCache::Jit_PosS8Skin() {
	fp.SXTL(8, D13, S13);
	fp.SXTL(16, Q13, D13);
	fp.SCVTF(32, Q13, Q13, 7);
	Jit_SkinTransform();
}

void VertexDecoderJitCache::Jit_PosS16Skin() {
	fp.SXTL(16, Q13, D13);
	fp.SCVTF(32, Q13, Q13, 15);
	Jit_SkinTransform();
}

void VertexDecoderJitCache::Jit_PosFloatSkin() {
	Jit_SkinTransform();
}

void VertexDecoderJitCache::Jit_SkinTransform() {
	if (skinNormalPending_) {
		fp.FMUL(32, Q9, Q4, skinNormalReg_, 0);
		fp.FMUL(32, Q15, Q4, Q13, 0);
		fp.FMLA(32, Q9, Q5, skinNormalReg_, 1);
		fp.FMLA(32, Q15, Q5, Q13, 1);
		fp.FMLA(32, Q9, Q6, skinNormalReg_, 2);
		fp.FMLA(32, Q15, Q6, Q13, 2);
		fp.FADD(32, Q15, Q15, Q7);
		fp.STUR(128, Q9, dstReg, skinNormalOff_);
		fp.STUR(128, Q15, dstReg, dec_->decFmt.posoff);
		skinNormalPending_ = false;
	} else {
		fp.FMUL(32, Q15, Q4, Q13, 0);
		fp.FMLA(32, Q15, Q5, Q13, 1);
		fp.FMLA(32, Q15, Q6, Q13, 2);
		fp.FADD(32, Q15, Q15, Q7);
		fp.STUR(128, Q15, dstReg, dec_->decFmt.posoff);
	}
}

void VertexDecoderJitCache::Jit_AnyS8ToFloat(int srcoff) {
	fp.LDUR(32, src[0], srcReg, srcoff);
	fp.SXTL(8, srcD, src[0]);
	fp.SXTL(16, srcQ, srcD);
	fp.SCVTF(32, srcQ, srcQ, 7);
}

void VertexDecoderJitCache::Jit_AnyS16ToFloat(int srcoff) {
	fp.LDUR(64, src[0], srcReg, srcoff);
	fp.SXTL(16, srcQ, srcD);
	fp.SCVTF(32, srcQ, srcQ, 15);
}

void VertexDecoderJitCache::Jit_WriteMatrixMul(int outOff, bool pos) {
	// Multiply srcQ with the matrix sitting in Q4-Q7.
	fp.FMUL(32, accNEON, Q4, srcQ, 0);
	fp.FMLA(32, accNEON, Q5, srcQ, 1);
	fp.FMLA(32, accNEON, Q6, srcQ, 2);
	if (pos) {
		fp.FADD(32, accNEON, accNEON, Q7);
	}
	fp.STUR(128, accNEON, dstReg, outOff);
}

// ---- Deadstation Portable: two vertices a loop ----
//
// The one-vertex loop above runs each step's instructions as a chain: on the in-order A53 every load, conversion and multiply
// waits for the one before it. Here each component's instructions for vertex A and for vertex B go into lists of their own, with
// registers of their own, and the lists are emitted one instruction from each in turn: two independent chains (and integer
// work beside vector work) for the core to dual-issue. The loads, conversions and stores are the old steps' (the same widths at
// the same offsets, in the same order: A's stores, then B's), so the bytes written are the same, those past a vertex too; the
// texture coordinates are converted from general registers (one cycle a value instead of two 128-bit widenings), with the same
// fused multiply-add per lane. Measured against the old loop's code on the Brick (local/brick/vdecbench/skinbench.c).

namespace {

typedef std::function<void()> PairOp;

struct PairLane {
	int s, d;              // this vertex's offsets from srcReg and dstReg
	ARM64Reg uvU, uvV;     // W: raw texture coordinates, or float ones copied
	ARM64Reg c1, c2, c3;   // W: color
	ARM64Reg n1, n2;       // W: an s8 or s16 normal
	ARM64Reg fU, fV;       // S: texture coordinates converted (D views: the float prescale path)
	ARM64Reg pos, pos2;    // V: the position (S/D/Q views), and a 12-byte float copy's last word
	ARM64Reg nrm, nrm2;    // V: a float normal, and its 12-byte copy's last word
};

// Lane A in W3-W9 and V18/V19/V22/V24/V26/V27, lane B in W10, W11, W13-W17 and V20/V21/V23/V25/V28/V29: none of them saved
// registers; W12 (the alpha flag), W2, X0, X1, V0, V1 and V16/V17 (v's scale and offset) are shared.
const PairLane kPairLanes[2] = {
	{ 0, 0, W6, W7, W3, W4, W5, W8, W9, S18, S19, S22, S24, S26, S27 },
	{ 0, 0, W14, W15, W10, W11, W13, W16, W17, S20, S21, S23, S25, S28, S29 },
};
// lane B when texture bounds are tracked in W13-W16 (X19-X22 saved by the prologue then)
const PairLane kPairLaneBWithBounds = { 0, 0, W19, W20, W10, W11, W21, W22, W17, S20, S21, S23, S25, S28, S29 };

bool IsStep(const VertexDecoder &dec, int i, StepFunction f) {
	return dec.steps_[i] == f;
}

}  // namespace

bool VertexDecoderJitCache::PairSupported(const VertexDecoder &dec) const {
	if (dec.morphcount != 1)
		return false;
	typedef VertexDecoder V;
	if (dec.skinInDecode) {
		if (dec.nweights < 1 || dec.nweights > 8 || dec.numSteps_ < 2 || dec.weightoff != 0)
			return false;
		if (!IsStep(dec, 0, &V::Step_WeightsU8Skin) && !IsStep(dec, 0, &V::Step_WeightsU16Skin) && !IsStep(dec, 0, &V::Step_WeightsFloatSkin))
			return false;
		// float weights (3 or more) and a position, nothing else: no conversions for the pair to overlap, and bones 2 and 3 read
		// from memory instead of kept in registers: measured slower (P f W f x4: 60.0 -> 66.1 cycles a vertex in 7-vertex calls)
		if (IsStep(dec, 0, &V::Step_WeightsFloatSkin) && dec.nweights >= 3 && dec.numSteps_ == 2)
			return false;
		static const StepFunction okSkin[] = {
			&V::Step_TcU8Prescale, &V::Step_TcU16Prescale, &V::Step_TcFloatPrescale, &V::Step_TcU8ToFloat, &V::Step_TcU16ToFloat,
			&V::Step_TcFloat,
			&V::Step_Color8888, &V::Step_Color4444, &V::Step_Color565, &V::Step_Color5551,
			&V::Step_NormalS8Skin, &V::Step_NormalS16Skin, &V::Step_NormalFloatSkin,
			&V::Step_PosS8Skin, &V::Step_PosS16Skin, &V::Step_PosFloatSkin,
		};
		bool hasPos = false;
		for (int i = 1; i < dec.numSteps_; i++) {
			bool found = false;
			for (StepFunction f : okSkin)
				found = found || dec.steps_[i] == f;
			if (!found)
				return false;
			hasPos = hasPos || IsStep(dec, i, &V::Step_PosS8Skin) || IsStep(dec, i, &V::Step_PosS16Skin) || IsStep(dec, i, &V::Step_PosFloatSkin);
		}
		return hasPos && dec.steps_[dec.numSteps_ - 1] != nullptr && (IsStep(dec, dec.numSteps_ - 1, &V::Step_PosS8Skin) ||
			IsStep(dec, dec.numSteps_ - 1, &V::Step_PosS16Skin) || IsStep(dec, dec.numSteps_ - 1, &V::Step_PosFloatSkin));
	}
	static const StepFunction ok[] = {
		&V::Step_TcU8Prescale, &V::Step_TcU16Prescale, &V::Step_TcFloatPrescale, &V::Step_TcU8ToFloat, &V::Step_TcU16ToFloat,
		&V::Step_TcFloat, &V::Step_TcFloatThrough, &V::Step_TcU16ThroughToFloat,
		&V::Step_Color8888, &V::Step_Color4444, &V::Step_Color565, &V::Step_Color5551,
		&V::Step_NormalS8, &V::Step_NormalS16, &V::Step_NormalFloat,
		&V::Step_PosS8, &V::Step_PosS16, &V::Step_PosFloat, &V::Step_PosS8Through, &V::Step_PosS16Through, &V::Step_PosFloatThrough,
	};
	bool through = false, normal = false;
	for (int i = 0; i < dec.numSteps_; i++) {
		bool found = false;
		for (StepFunction f : ok)
			found = found || dec.steps_[i] == f;
		if (!found)
			return false;
		through = through || IsStep(dec, i, &V::Step_PosS8Through) || IsStep(dec, i, &V::Step_PosS16Through) || IsStep(dec, i, &V::Step_PosFloatThrough);
		normal = normal || IsStep(dec, i, &V::Step_NormalS8) || IsStep(dec, i, &V::Step_NormalS16) || IsStep(dec, i, &V::Step_NormalFloat);
	}
	// a through-mode position's z goes through a normal's register
	return !(through && normal);
}

void VertexDecoderJitCache::Jit_Pair(const VertexDecoder &dec) {
	typedef VertexDecoder V;
	bool bounds = false;
	for (int i = 0; i < dec.numSteps_; i++)
		bounds = bounds || IsStep(dec, i, &V::Step_TcU16ThroughToFloat);
	PairLane lanes[2] = { kPairLanes[0], bounds ? kPairLaneBWithBounds : kPairLanes[1] };
	lanes[1].s = dec.VertexSize();
	lanes[1].d = dec.decFmt.stride;
	std::vector<PairOp> lists[9];  // per lane: texture coordinates, color, normal, position; then the bounds (both lanes, in turn)
	std::vector<PairOp> stores[2];
	auto updateSide = [this](ARM64Reg src, CCFlags cc, ARM64Reg dst) {
		CMP(src, dst);
		CSEL(dst, src, dst, cc);
	};

	for (int k = 0; k < 2; k++) {
		const PairLane R = lanes[k];
		std::vector<PairOp> &tc = lists[k], &col = lists[2 + k], &nrm = lists[4 + k], &pos = lists[6 + k];
		std::vector<PairOp> &st = stores[k];
		for (int i = 0; i < dec.numSteps_; i++) {
			const bool tcU8Pre = IsStep(dec, i, &V::Step_TcU8Prescale), tcU16Pre = IsStep(dec, i, &V::Step_TcU16Prescale);
			const bool tcU8 = IsStep(dec, i, &V::Step_TcU8ToFloat), tcU16 = IsStep(dec, i, &V::Step_TcU16ToFloat);
			if (tcU8Pre || tcU16Pre || tcU8 || tcU16) {
				const bool wide = tcU16Pre || tcU16, pre = tcU8Pre || tcU16Pre;
				const int off = R.s + dec.tcoff, fbits = wide ? 15 : 7;
				tc.push_back([=] { if (wide) LDURH(R.uvU, srcReg, off); else LDURB(R.uvU, srcReg, off); });
				tc.push_back([=] { if (wide) LDURH(R.uvV, srcReg, off + 2); else LDURB(R.uvV, srcReg, off + 1); });
				tc.push_back([=] { fp.UCVTF(R.fU, R.uvU, fbits); });
				tc.push_back([=] { fp.UCVTF(R.fV, R.uvV, fbits); });
				if (pre) {
					tc.push_back([=] { fp.FMADD(R.fU, R.fU, S0, S1); });     // offset u + u * scale u
					tc.push_back([=] { fp.FMADD(R.fV, R.fV, S16, S17); });   // offset v + v * scale v
				}
				st.push_back([=] { fp.STP(32, INDEX_SIGNED, R.fU, R.fV, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_TcFloatPrescale)) {
				const ARM64Reg raw = EncodeRegToDouble(R.fU), out = EncodeRegToDouble(R.fV);
				tc.push_back([=] { fp.LDUR(64, raw, srcReg, R.s + dec.tcoff); });
				tc.push_back([=] { fp.MOV(out, neonUVOffsetReg); });
				tc.push_back([=] { fp.FMLA(32, out, raw, neonUVScaleReg); });
				st.push_back([=] { fp.STUR(64, out, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_TcFloat) || IsStep(dec, i, &V::Step_TcFloatThrough)) {
				tc.push_back([=] { LDP(INDEX_SIGNED, R.uvU, R.uvV, srcReg, R.s + dec.tcoff); });
				st.push_back([=] { STP(INDEX_SIGNED, R.uvU, R.uvV, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_TcU16ThroughToFloat)) {
				// the raw values (the old step's two loads), converted one each (the same values: integers to floats), the bounds
				// updated in their own list (A's, then B's: compare and select pairs, nothing else between them sets flags)
				const int off = R.s + dec.tcoff;
				tc.push_back([=] { LDURH(R.uvU, srcReg, off); });
				tc.push_back([=] { LDURH(R.uvV, srcReg, off + 2); });
				tc.push_back([=] { fp.UCVTF(R.fU, R.uvU); });
				tc.push_back([=] { fp.UCVTF(R.fV, R.uvV); });
				std::vector<PairOp> &b = lists[8];
				b.push_back([=] { updateSide(R.uvU, CC_LT, boundsMinUReg); });
				b.push_back([=] { updateSide(R.uvU, CC_GT, boundsMaxUReg); });
				b.push_back([=] { updateSide(R.uvV, CC_LT, boundsMinVReg); });
				b.push_back([=] { updateSide(R.uvV, CC_GT, boundsMaxVReg); });
				st.push_back([=] { fp.STP(32, INDEX_SIGNED, R.fU, R.fV, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_PosS8Through)) {
				// 8-bit positions in through mode always decode to 0, depth included
				st.push_back([=] { STUR(WZR, dstReg, R.d + dec.decFmt.posoff); });
				st.push_back([=] { STUR(WZR, dstReg, R.d + dec.decFmt.posoff + 4); });
				st.push_back([=] { STUR(WZR, dstReg, R.d + dec.decFmt.posoff + 8); });
			} else if (IsStep(dec, i, &V::Step_PosS16Through)) {
				// x and y (signed), then z (unsigned) through a general register, as the old step
				pos.push_back([=] { fp.LDUR(32, R.pos, srcReg, R.s + dec.posoff); });
				pos.push_back([=] { LDURH(R.n1, srcReg, R.s + dec.posoff + 4); });
				pos.push_back([=] { fp.SXTL(16, EncodeRegToDouble(R.pos), R.pos); });
				pos.push_back([=] { fp.SCVTF(32, EncodeRegToDouble(R.pos), EncodeRegToDouble(R.pos)); });
				pos.push_back([=] { fp.SCVTF(R.pos2, R.n1); });
				st.push_back([=] { fp.STUR(64, EncodeRegToDouble(R.pos), dstReg, R.d + dec.decFmt.posoff); });
				st.push_back([=] { fp.STUR(32, R.pos2, dstReg, R.d + dec.decFmt.posoff + 8); });
			} else if (IsStep(dec, i, &V::Step_PosFloatThrough)) {
				// x and y copied, z clamped to 0-65535 as the old step does
				const ARM64Reg z = EncodeRegToDouble(R.pos2);
				pos.push_back([=] { fp.LDUR(64, EncodeRegToDouble(R.pos), srcReg, R.s + dec.posoff); });
				pos.push_back([=] { fp.LDUR(32, z, srcReg, R.s + dec.posoff + 8); });
				pos.push_back([=] { fp.FCVTZU(32, z, z); });
				pos.push_back([=] { fp.UQXTN(16, z, z); });
				pos.push_back([=] { fp.UXTL(16, z, z); });
				pos.push_back([=] { fp.UCVTF(32, z, z); });
				st.push_back([=] { fp.STUR(64, EncodeRegToDouble(R.pos), dstReg, R.d + dec.decFmt.posoff); });
				st.push_back([=] { fp.STUR(32, z, dstReg, R.d + dec.decFmt.posoff + 8); });
			} else if (IsStep(dec, i, &V::Step_Color8888)) {
				col.push_back([=] { LDUR(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st.push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color4444)) {
				col.push_back([=] { LDURH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x000F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x00F0); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 4)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x0F00); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 8)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0xF000); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 12)); });
				col.push_back([=] { ORR(R.c1, R.c2, R.c2, ArithOption(R.c2, ST_LSL, 4)); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st.push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color565)) {
				col.push_back([=] { LDURH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x001F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0xF800); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 5)); });
				col.push_back([=] { LSL(R.c3, R.c2, 3); });
				col.push_back([=] { ORR(R.c2, R.c3, R.c2, ArithOption(R.c2, ST_LSR, 2)); });
				col.push_back([=] { ANDI2R(R.c2, R.c2, 0xFFFF00FF); });
				col.push_back([=] { LSR(R.c1, R.c1, 5); });
				col.push_back([=] { ANDI2R(R.c1, R.c1, 0x003F); });
				col.push_back([=] { LSL(R.c3, R.c1, 2); });
				col.push_back([=] { ORR(R.c3, R.c3, R.c1, ArithOption(R.c1, ST_LSR, 4)); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 8)); });
				col.push_back([=] { ORRI2R(R.c1, R.c2, 0xFF000000); });
				st.push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color5551)) {
				col.push_back([=] { LDURSH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x001F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x03E0); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 3)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x7C00); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 6)); });
				col.push_back([=] { LSR(R.c3, R.c2, 2); });
				col.push_back([=] { ANDI2R(R.c3, R.c3, ~0x000000F8); });
				col.push_back([=] { ANDI2R(R.c3, R.c3, ~0x0000F800); });
				col.push_back([=] { ORR(R.c2, R.c3, R.c2, ArithOption(R.c2, ST_LSL, 3)); });
				col.push_back([=] { ANDI2R(R.c1, R.c1, 0xFF000000); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c1); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st.push_back([=] { STUR(R.c2, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_NormalS8)) {
				nrm.push_back([=] { LDURH(R.n1, srcReg, R.s + dec.nrmoff); });
				nrm.push_back([=] { LDURB(R.n2, srcReg, R.s + dec.nrmoff + 2); });
				nrm.push_back([=] { ORR(R.n1, R.n1, R.n2, ArithOption(R.n2, ST_LSL, 16)); });
				st.push_back([=] { STUR(R.n1, dstReg, R.d + dec.decFmt.nrmoff); });
			} else if (IsStep(dec, i, &V::Step_NormalS16)) {
				nrm.push_back([=] { LDUR(R.n1, srcReg, R.s + dec.nrmoff); });
				nrm.push_back([=] { LDURH(R.n2, srcReg, R.s + dec.nrmoff + 4); });
				st.push_back([=] { STP(INDEX_SIGNED, R.n1, R.n2, dstReg, R.d + dec.decFmt.nrmoff); });
			} else if (IsStep(dec, i, &V::Step_NormalFloat)) {
				// as the old step: 16 bytes when both offsets are 8-aligned, else 12
				if ((dec.nrmoff & 7) == 0 && (dec.decFmt.nrmoff & 7) == 0) {
					nrm.push_back([=] { fp.LDUR(128, EncodeRegToQuad(R.nrm), srcReg, R.s + dec.nrmoff); });
					st.push_back([=] { fp.STUR(128, EncodeRegToQuad(R.nrm), dstReg, R.d + dec.decFmt.nrmoff); });
				} else {
					nrm.push_back([=] { fp.LDUR(64, EncodeRegToDouble(R.nrm), srcReg, R.s + dec.nrmoff); });
					nrm.push_back([=] { fp.LDUR(32, R.nrm2, srcReg, R.s + dec.nrmoff + 8); });
					st.push_back([=] { fp.STUR(64, EncodeRegToDouble(R.nrm), dstReg, R.d + dec.decFmt.nrmoff); });
					st.push_back([=] { fp.STUR(32, R.nrm2, dstReg, R.d + dec.decFmt.nrmoff + 8); });
				}
			} else if (IsStep(dec, i, &V::Step_PosS8)) {
				pos.push_back([=] { fp.LDUR(32, R.pos, srcReg, R.s + dec.posoff); });
				pos.push_back([=] { fp.SXTL(8, EncodeRegToDouble(R.pos), R.pos); });
				pos.push_back([=] { fp.SXTL(16, EncodeRegToQuad(R.pos), EncodeRegToDouble(R.pos)); });
				pos.push_back([=] { fp.SCVTF(32, EncodeRegToQuad(R.pos), EncodeRegToQuad(R.pos), 7); });
				st.push_back([=] { fp.STUR(128, EncodeRegToQuad(R.pos), dstReg, R.d + dec.decFmt.posoff); });
			} else if (IsStep(dec, i, &V::Step_PosS16)) {
				pos.push_back([=] { fp.LDUR(64, EncodeRegToDouble(R.pos), srcReg, R.s + dec.posoff); });
				pos.push_back([=] { fp.SXTL(16, EncodeRegToQuad(R.pos), EncodeRegToDouble(R.pos)); });
				pos.push_back([=] { fp.SCVTF(32, EncodeRegToQuad(R.pos), EncodeRegToQuad(R.pos), 15); });
				st.push_back([=] { fp.STUR(128, EncodeRegToQuad(R.pos), dstReg, R.d + dec.decFmt.posoff); });
			} else if (IsStep(dec, i, &V::Step_PosFloat)) {
				if ((dec.posoff & 7) == 0 && (dec.decFmt.posoff & 7) == 0) {
					pos.push_back([=] { fp.LDUR(128, EncodeRegToQuad(R.pos), srcReg, R.s + dec.posoff); });
					st.push_back([=] { fp.STUR(128, EncodeRegToQuad(R.pos), dstReg, R.d + dec.decFmt.posoff); });
				} else {
					pos.push_back([=] { fp.LDUR(64, EncodeRegToDouble(R.pos), srcReg, R.s + dec.posoff); });
					pos.push_back([=] { fp.LDUR(32, R.pos2, srcReg, R.s + dec.posoff + 8); });
					st.push_back([=] { fp.STUR(64, EncodeRegToDouble(R.pos), dstReg, R.d + dec.decFmt.posoff); });
					st.push_back([=] { fp.STUR(32, R.pos2, dstReg, R.d + dec.decFmt.posoff + 8); });
				}
			}
		}
	}

	// one instruction from each list in turn
	size_t at[9]{};
	for (bool any = true; any;) {
		any = false;
		for (int l = 0; l < 9; l++) {
			if (at[l] < lists[l].size()) {
				lists[l][at[l]++]();
				any = true;
			}
		}
	}
	for (const PairOp &op : stores[0])
		op();
	for (const PairOp &op : stores[1])
		op();
}

// Skinned, two vertices a loop, in phases (registers are reused between them, so phases follow one another; within one, the
// lists are emitted one instruction from each in turn):
//   1. the weights loaded and converted, the normal's and position's bytes loaded, the texture coordinates' bytes loaded;
//   2. both blended matrices, bone by bone, A's and B's multiplies alternating (a bone from 2 up read once from the array for
//      both), beside the colors' integer work;
//   3. texture coordinates converted (into the weights' registers, free now), the normal and position converted;
//   4. the four transforms (A's normal and position, B's), their multiplies alternating;
//   5. A's stores, then B's, in the old order.
// The old loop's operations in its order for each vertex (matrix = bone0 * w0, then + bone k * wk fused, in k order; then the
// old transforms), so the same bits. Registers: V0/V1 the uv scale and offset pairs, V2/V3 v's as scalars, V16-V23 bones 0 and
// 1, V28-V31 bone k >= 2 in phase 2 and the transforms' accumulators in 4. A: V4-V7 matrix, V8 normal, V9 position, V12/V13
// weights then uv; B: V24-V27, V10, V11, V14/V15. GPRs: A's color W3-W5, uv W6/W7, one or two scalar weights W8/W9; B's W10, W11,
// W13, W14/W15, W16/W17; X8 points at bone 2 when there are three or more.
void VertexDecoderJitCache::Jit_PairSkinned(const VertexDecoder &dec) {
	typedef VertexDecoder V;
	const int n = dec.nweights;
	const bool wU8 = IsStep(dec, 0, &V::Step_WeightsU8Skin), wU16 = IsStep(dec, 0, &V::Step_WeightsU16Skin);
	const bool scalarWeights = (wU8 || wU16) && n <= 2;

	struct Lane {
		int s, d;
		ARM64Reg M[4];
		ARM64Reg nrm, pos;        // V (S/D/Q views)
		ARM64Reg w0, w1;          // V: weights (vector: lanes; scalar: w0's lane 0 and w1's lane 0), then uv (S)
		ARM64Reg gw0, gw1;        // W: scalar weights' bytes
		ARM64Reg uvU, uvV;        // W
		ARM64Reg c1, c2, c3;      // W
		ARM64Reg accN, accP;      // V
	};
	Lane L[2] = {
		{ 0, 0, { Q4, Q5, Q6, Q7 }, Q8, Q9, Q12, Q13, W8, W9, W6, W7, W3, W4, W5, Q28, Q29 },
		{ dec.VertexSize(), dec.decFmt.stride, { Q24, Q25, Q26, Q27 }, Q10, Q11, Q14, Q15, W16, W17, W14, W15, W10, W11, W13, Q30, Q31 },
	};
	const ARM64Reg bone0[4] = { Q16, Q17, Q18, Q19 }, bone1[4] = { Q20, Q21, Q22, Q23 }, boneK[4] = { Q28, Q29, Q30, Q31 };
	// weight k of a lane: its register and lane
	auto weight = [&](const Lane &R, int k, ARM64Reg *reg, u8 *idx) {
		if (scalarWeights) {
			*reg = k == 0 ? R.w0 : R.w1;
			*idx = 0;
		} else {
			*reg = k < 4 ? R.w0 : R.w1;
			*idx = (u8)(k & 3);
		}
	};

	std::vector<PairOp> p1[6], p2[3], p3[4], p4, st[2];
	ARM64Reg normalStep = INVALID_REG;  // set when there is a normal
	bool hasNormal = false;
	int nrmKind = 0, posKind = 0;  // 8, 16, 32 (float)
	for (int i = 1; i < dec.numSteps_; i++) {
		if (IsStep(dec, i, &V::Step_NormalS8Skin)) { hasNormal = true; nrmKind = 8; }
		else if (IsStep(dec, i, &V::Step_NormalS16Skin)) { hasNormal = true; nrmKind = 16; }
		else if (IsStep(dec, i, &V::Step_NormalFloatSkin)) { hasNormal = true; nrmKind = 32; }
		else if (IsStep(dec, i, &V::Step_PosS8Skin)) posKind = 8;
		else if (IsStep(dec, i, &V::Step_PosS16Skin)) posKind = 16;
		else if (IsStep(dec, i, &V::Step_PosFloatSkin)) posKind = 32;
	}
	(void)normalStep;

	for (int k = 0; k < 2; k++) {
		const Lane R = L[k];
		std::vector<PairOp> &w = p1[k], &raw = p1[2 + k], &uvRaw = p1[4 + k];
		std::vector<PairOp> &col = p2[1 + k], &uv = p3[k], &conv = p3[2 + k];
		// 1. weights
		if (wU8 || wU16) {
			if (scalarWeights) {
				const int fbits = wU8 ? 7 : 15;
				for (int j = 0; j < n; j++) {
					const ARM64Reg g = j == 0 ? R.gw0 : R.gw1, v = EncodeRegToSingle(j == 0 ? R.w0 : R.w1);
					const int off = R.s + (wU8 ? j : j * 2);
					w.push_back([=] { if (wU8) LDURB(g, srcReg, off); else LDURH(g, srcReg, off); });
					w.push_back([=] { fp.UCVTF(v, g, fbits); });
				}
			} else {
				// as the old step: the first four (over-read up to four), then the rest from +4 or +8
				for (int part = 0; part < (n > 4 ? 2 : 1); part++) {
					const int cnt = part == 0 ? std::min(n, 4) : n - 4;
					const ARM64Reg q = part == 0 ? R.w0 : R.w1, d = EncodeRegToDouble(q);
					if (wU8) {
						const int bits = cnt == 1 ? 8 : cnt == 2 ? 16 : 32;
						const int off = R.s + part * 4;
						w.push_back([=] {
							if (bits == 8) fp.LDUR(8, d, srcReg, off); else fp.LDUR(bits, d, srcReg, off);
						});
						w.push_back([=] { fp.UXTL(8, q, d); });
						w.push_back([=] { fp.UXTL(16, q, d); });
						w.push_back([=] { fp.UCVTF(32, q, q, 7); });
					} else {
						const int bits = cnt == 1 ? 16 : cnt == 2 ? 32 : 64;
						const int off = R.s + part * 8;
						w.push_back([=] { fp.LDUR(bits, d, srcReg, off); });
						w.push_back([=] { fp.UXTL(16, q, d); });
						w.push_back([=] { fp.UCVTF(32, q, q, 15); });
					}
				}
			}
		} else {
			// float weights, loaded as the old step does (the over-read lanes unused)
			const int bits0 = n == 1 ? 32 : n == 2 ? 64 : 128;
			const ARM64Reg q0 = R.w0, q1 = R.w1;
			w.push_back([=] { fp.LDUR(bits0, bits0 == 32 ? EncodeRegToSingle(q0) : bits0 == 64 ? EncodeRegToDouble(q0) : q0, srcReg, R.s); });
			if (n > 4) {
				const int bits1 = n == 5 ? 32 : n == 6 ? 64 : 128;
				w.push_back([=] { fp.LDUR(bits1, bits1 == 32 ? EncodeRegToSingle(q1) : bits1 == 64 ? EncodeRegToDouble(q1) : q1, srcReg, R.s + 16); });
			}
		}
		// 1. the normal's and position's bytes
		if (hasNormal) {
			const int off = R.s + dec.nrmoff;
			raw.push_back([=] {
				if (nrmKind == 8) fp.LDUR(32, EncodeRegToSingle(R.nrm), srcReg, off);
				else if (nrmKind == 16) fp.LDUR(64, EncodeRegToDouble(R.nrm), srcReg, off);
				else fp.LDUR(128, R.nrm, srcReg, off);
			});
		}
		{
			const int off = R.s + dec.posoff;
			raw.push_back([=] {
				if (posKind == 8) fp.LDUR(32, EncodeRegToSingle(R.pos), srcReg, off);
				else if (posKind == 16) fp.LDUR(64, EncodeRegToDouble(R.pos), srcReg, off);
				else fp.LDUR(128, R.pos, srcReg, off);
			});
		}
		// texture coordinates and color, in step order (their stores are the first ones, in that order)
		for (int i = 1; i < dec.numSteps_; i++) {
			const bool tcU8Pre = IsStep(dec, i, &V::Step_TcU8Prescale), tcU16Pre = IsStep(dec, i, &V::Step_TcU16Prescale);
			const bool tcU8 = IsStep(dec, i, &V::Step_TcU8ToFloat), tcU16 = IsStep(dec, i, &V::Step_TcU16ToFloat);
			const ARM64Reg fU = EncodeRegToSingle(R.w0), fV = EncodeRegToSingle(R.w1);
			if (tcU8Pre || tcU16Pre || tcU8 || tcU16) {
				const bool wide = tcU16Pre || tcU16, pre = tcU8Pre || tcU16Pre;
				const int off = R.s + dec.tcoff, fbits = wide ? 15 : 7;
				uvRaw.push_back([=] { if (wide) LDURH(R.uvU, srcReg, off); else LDURB(R.uvU, srcReg, off); });
				uvRaw.push_back([=] { if (wide) LDURH(R.uvV, srcReg, off + 2); else LDURB(R.uvV, srcReg, off + 1); });
				uv.push_back([=] { fp.UCVTF(fU, R.uvU, fbits); });
				uv.push_back([=] { fp.UCVTF(fV, R.uvV, fbits); });
				if (pre) {
					uv.push_back([=] { fp.FMADD(fU, fU, S0, S1); });
					uv.push_back([=] { fp.FMADD(fV, fV, S2, S3); });
				}
				st[k].push_back([=] { fp.STP(32, INDEX_SIGNED, fU, fV, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_TcFloatPrescale)) {
				const ARM64Reg rawD = EncodeRegToDouble(R.w0), out = EncodeRegToDouble(R.w1);
				uv.push_back([=] { fp.LDUR(64, rawD, srcReg, R.s + dec.tcoff); });
				uv.push_back([=] { fp.MOV(out, neonUVOffsetReg); });
				uv.push_back([=] { fp.FMLA(32, out, rawD, neonUVScaleReg); });
				st[k].push_back([=] { fp.STUR(64, out, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_TcFloat)) {
				uvRaw.push_back([=] { LDP(INDEX_SIGNED, R.uvU, R.uvV, srcReg, R.s + dec.tcoff); });
				st[k].push_back([=] { STP(INDEX_SIGNED, R.uvU, R.uvV, dstReg, R.d + dec.decFmt.uvoff); });
			} else if (IsStep(dec, i, &V::Step_Color8888)) {
				col.push_back([=] { LDUR(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st[k].push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color4444)) {
				col.push_back([=] { LDURH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x000F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x00F0); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 4)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x0F00); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 8)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0xF000); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 12)); });
				col.push_back([=] { ORR(R.c1, R.c2, R.c2, ArithOption(R.c2, ST_LSL, 4)); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st[k].push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color565)) {
				col.push_back([=] { LDURH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x001F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0xF800); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 5)); });
				col.push_back([=] { LSL(R.c3, R.c2, 3); });
				col.push_back([=] { ORR(R.c2, R.c3, R.c2, ArithOption(R.c2, ST_LSR, 2)); });
				col.push_back([=] { ANDI2R(R.c2, R.c2, 0xFFFF00FF); });
				col.push_back([=] { LSR(R.c1, R.c1, 5); });
				col.push_back([=] { ANDI2R(R.c1, R.c1, 0x003F); });
				col.push_back([=] { LSL(R.c3, R.c1, 2); });
				col.push_back([=] { ORR(R.c3, R.c3, R.c1, ArithOption(R.c1, ST_LSR, 4)); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 8)); });
				col.push_back([=] { ORRI2R(R.c1, R.c2, 0xFF000000); });
				st[k].push_back([=] { STUR(R.c1, dstReg, R.d + dec.decFmt.c0off); });
			} else if (IsStep(dec, i, &V::Step_Color5551)) {
				col.push_back([=] { LDURSH(R.c1, srcReg, R.s + dec.coloff); });
				col.push_back([=] { ANDI2R(R.c2, R.c1, 0x001F); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x03E0); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 3)); });
				col.push_back([=] { ANDI2R(R.c3, R.c1, 0x7C00); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c3, ArithOption(R.c3, ST_LSL, 6)); });
				col.push_back([=] { LSR(R.c3, R.c2, 2); });
				col.push_back([=] { ANDI2R(R.c3, R.c3, ~0x000000F8); });
				col.push_back([=] { ANDI2R(R.c3, R.c3, ~0x0000F800); });
				col.push_back([=] { ORR(R.c2, R.c3, R.c2, ArithOption(R.c2, ST_LSL, 3)); });
				col.push_back([=] { ANDI2R(R.c1, R.c1, 0xFF000000); });
				col.push_back([=] { ORR(R.c2, R.c2, R.c1); });
				col.push_back([=] { ORN(alphaNonFullReg, alphaNonFullReg, R.c1, ArithOption(R.c1, ST_ASR, 24)); });
				st[k].push_back([=] { STUR(R.c2, dstReg, R.d + dec.decFmt.c0off); });
			}
		}
		// 3. the normal and position converted (as the old steps)
		if (hasNormal && nrmKind != 32) {
			const ARM64Reg q = R.nrm, d = EncodeRegToDouble(R.nrm), s1 = EncodeRegToSingle(R.nrm);
			if (nrmKind == 8) {
				conv.push_back([=] { fp.SXTL(8, d, s1); });
				conv.push_back([=] { fp.SXTL(16, q, d); });
				conv.push_back([=] { fp.SCVTF(32, q, q, 7); });
			} else {
				conv.push_back([=] { fp.SXTL(16, q, d); });
				conv.push_back([=] { fp.SCVTF(32, q, q, 15); });
			}
		}
		if (posKind != 32) {
			const ARM64Reg q = R.pos, d = EncodeRegToDouble(R.pos), s1 = EncodeRegToSingle(R.pos);
			if (posKind == 8) {
				conv.push_back([=] { fp.SXTL(8, d, s1); });
				conv.push_back([=] { fp.SXTL(16, q, d); });
				conv.push_back([=] { fp.SCVTF(32, q, q, 7); });
			} else {
				conv.push_back([=] { fp.SXTL(16, q, d); });
				conv.push_back([=] { fp.SCVTF(32, q, q, 15); });
			}
		}
		// 5. the normal's and position's stores (the old transform's, last)
		if (hasNormal)
			st[k].push_back([=] { fp.STUR(128, R.accN, dstReg, R.d + dec.decFmt.nrmoff); });
		st[k].push_back([=] { fp.STUR(128, R.accP, dstReg, R.d + dec.decFmt.posoff); });
	}

	// 2. both matrices, bone by bone
	std::vector<PairOp> &mat = p2[0];
	for (int b = 0; b < n; b++) {
		const ARM64Reg *bone = b == 0 ? bone0 : b == 1 ? bone1 : boneK;
		if (b >= 2) {
			const int off = (b - 2) * 64;
			mat.push_back([=] { fp.LDP(128, INDEX_SIGNED, Q28, Q29, X8, off); });
			mat.push_back([=] { fp.LDP(128, INDEX_SIGNED, Q30, Q31, X8, off + 32); });
		}
		for (int c = 0; c < 4; c++) {
			for (int k = 0; k < 2; k++) {
				ARM64Reg wr; u8 wi;
				weight(L[k], b, &wr, &wi);
				const ARM64Reg m = L[k].M[c], br = bone[c];
				if (b == 0)
					mat.push_back([=] { fp.FMUL(32, m, br, wr, wi); });
				else
					mat.push_back([=] { fp.FMLA(32, m, br, wr, wi); });
			}
		}
	}
	// 4. the transforms: the old order for each accumulator, the four alternating
	for (int j = 0; j < 3; j++) {
		for (int k = 0; k < 2; k++) {
			const Lane &R = L[k];
			const ARM64Reg mj = R.M[j], nrm = R.nrm, pos = R.pos, aN = R.accN, aP = R.accP;
			const u8 jj = (u8)j;
			if (hasNormal)
				p4.push_back([=] { if (jj == 0) fp.FMUL(32, aN, mj, nrm, 0); else fp.FMLA(32, aN, mj, nrm, jj); });
			p4.push_back([=] { if (jj == 0) fp.FMUL(32, aP, mj, pos, 0); else fp.FMLA(32, aP, mj, pos, jj); });
		}
	}
	for (int k = 0; k < 2; k++) {
		const Lane &R = L[k];
		const ARM64Reg aP = R.accP, m3 = R.M[3];
		p4.push_back([=] { fp.FADD(32, aP, aP, m3); });
	}

	auto emitPhase = [](std::vector<PairOp> *lists, int count) {
		size_t at[8]{};
		for (bool any = true; any;) {
			any = false;
			for (int l = 0; l < count; l++) {
				if (at[l] < lists[l].size()) {
					lists[l][at[l]++]();
					any = true;
				}
			}
		}
	};
	emitPhase(p1, 6);
	emitPhase(p2, 3);
	emitPhase(p3, 4);
	for (const PairOp &op : p4)
		op();
	for (const PairOp &op : st[0])
		op();
	for (const PairOp &op : st[1])
		op();
}
