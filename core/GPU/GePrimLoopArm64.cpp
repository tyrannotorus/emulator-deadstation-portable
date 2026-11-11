// Deadstation Portable: Execute_Prim's lookahead (the commands after a PRIM, read on from it) in hand-written ARM64. Upstream's C++
// loop is the rule it keeps (draws merged and queued, state words kept, the GE time charged); a command it doesn't take goes back to
// Execute_Prim untouched (PrimLoop: s->src at it; s->bail where the lookahead ends). Field offsets come from the compiler (offsetof).
//
// void GPUCommonHW::PrimLoopArm64(GPUCommonHW *gpu, PrimLoop *s): the state's pointers come in s
// Registers: x19 s, x20 gstate.cmdmem, x21 src, x22 stall, x23 the draw engine, x24 gstate_c, x25 its batch fields, x26 the
// decoder, w27 the vertex type, w28 its decoder's ID. Across a call into C++ the PRIM's own values wait at [sp, #96]; the gpu
// is at [sp, #112].
#include "ppsspp_config.h"
#if PPSSPP_ARCH(ARM64)

#include <cstddef>
#include "GPU/GPUCommonHW.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/VramAlias.h"

// The draw engine, the decoder and gstate_c aren't standard-layout (virtual functions, mixed access), so offsetof warns; for
// their plain data members, with one base class at most, it's the offset the compiler uses itself.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

// the command's case below, by its number (0: the lookahead ends there)
struct Cases {
	u8 at[256] {};
	constexpr Cases() {
		at[GE_CMD_PRIM] = 1;
		at[GE_CMD_VERTEXTYPE] = 2;
		at[GE_CMD_VADDR] = 3;
		at[GE_CMD_IADDR] = 4;
		at[GE_CMD_OFFSETADDR] = 5;
		at[GE_CMD_BASE] = 6;
		at[GE_CMD_TEXFLUSH] = 6;
		at[GE_CMD_NOP] = 6;
		at[GE_CMD_NOP_FF] = 6;
		at[GE_CMD_BONEMATRIXNUMBER] = 6;
		at[GE_CMD_CULLFACEENABLE] = 7;
		at[GE_CMD_CULL] = 8;
		at[GE_CMD_TEXSCALEU] = 9;
		at[GE_CMD_TEXSCALEV] = 9;
		at[GE_CMD_TEXOFFSETU] = 9;
		at[GE_CMD_TEXOFFSETV] = 9;
		at[GE_CMD_TEXLEVEL] = 10;
		at[GE_CMD_CALL] = 11;
		at[GE_CMD_TEXBUFWIDTH0] = 12;
		at[GE_CMD_TEXADDR0] = 12;
	}
};
extern "C" const Cases GePrimLoopCases;
const Cases GePrimLoopCases;
static_assert(sizeof(Cases) == 256, "a byte a command");

// what it leaves to C++ in the middle of a command
extern "C" bool GePrimCullTest(DrawEngineCommon *engine, const void *verts, int count, const VertexDecoder *dec, u32 vertType) {
	return engine->TestBoundingBoxFast(verts, count, dec, vertType);
}
extern "C" void GePrimFlushSkin(DrawEngineCommon *engine) {
	engine->FlushSkin();
}
// ReadThroughUploads' vertices: where the ones just copied into VRAM are read from
extern "C" u32 GePrimReadable(u32 addr, u32 size) {
	return g_vramAlias.readable(addr, size);
}
extern "C" bool GePrimBoneCall(GPUCommonHW *gpu, u32 data) {
	return gpu->PrimLoopBoneCall(data);
}

__attribute__((naked)) void GPUCommonHW::PrimLoopArm64(GPUCommonHW *gpu, PrimLoop *s) {
	__asm__(
		".set PL_src, %c[src]\n\t.set PL_stall, %c[stall]\n\t.set PL_decoder, %c[decoder]\n\t.set PL_base, %c[base]\n\t"
		".set PL_culled, %c[culled]\n\t.set PL_vtype, %c[vtype]\n\t.set PL_vtid, %c[vtid]\n\t.set PL_vtmask, %c[vtmask]\n\t"
		".set PL_idHigh, %c[idHigh]\n\t.set PL_cullMode, %c[cullMode]\n\t.set PL_total, %c[total]\n\t.set PL_bytesRead, %c[bytesRead]\n\t"
		".set PL_canExtend, %c[canExtend]\n\t.set PL_isTriangle, %c[isTriangle]\n\t.set PL_onePassed, %c[onePassed]\n\t"
		".set PL_vramPending, %c[vramPending]\n\t.set PL_bail, %c[bail]\n\t.set PL_cmdmem, %c[cmdmem]\n\t.set PL_gsc, %c[gsc]\n\t"
		".set PL_engine, %c[engine]\n"
		:: [src] "i"(offsetof(PrimLoop, src)), [stall] "i"(offsetof(PrimLoop, stall)), [decoder] "i"(offsetof(PrimLoop, decoder)), [base] "i"(offsetof(PrimLoop, base)),
		[culled] "i"(offsetof(PrimLoop, numCulledDraws)), [vtype] "i"(offsetof(PrimLoop, vertexType)), [vtid] "i"(offsetof(PrimLoop, vertTypeID)),
		[vtmask] "i"(offsetof(PrimLoop, vtypeCheckMask)), [idHigh] "i"(offsetof(PrimLoop, idHigh)), [cullMode] "i"(offsetof(PrimLoop, cullMode)),
		[total] "i"(offsetof(PrimLoop, totalVertCount)), [bytesRead] "i"(offsetof(PrimLoop, bytesRead)), [canExtend] "i"(offsetof(PrimLoop, canExtend)),
		[isTriangle] "i"(offsetof(PrimLoop, isTriangle)), [onePassed] "i"(offsetof(PrimLoop, onePassed)), [vramPending] "i"(offsetof(PrimLoop, vramPending)),
		[bail] "i"(offsetof(PrimLoop, bail)), [cmdmem] "i"(offsetof(PrimLoop, cmdmem)), [gsc] "i"(offsetof(PrimLoop, gstateC)),
		[engine] "i"(offsetof(PrimLoop, engine)));
	__asm__(
		".set DE_numVerts, %c[numVerts]\n\t.set DE_numInds, %c[numInds]\n\t.set DE_inCalls, %c[inCalls]\n\t.set DE_verts, %c[verts]\n\t"
		".set DE_inds, %c[inds]\n\t.set DE_prevPrim, %c[prevPrim]\n\t.set DE_dec, %c[dec]\n\t.set DE_lastVType, %c[lastVType]\n\t"
		".set DE_seen, %c[seen]\n\t.set DE_anyCCW, %c[anyCCW]\n\t.set DE_bInds, %c[bInds]\n\t.set DE_bCount, %c[bCount]\n\t"
		".set DE_bType, %c[bType]\n\t.set DE_bLower, %c[bLower]\n\t.set DE_bUpper, %c[bUpper]\n\t.set DE_lastDecType, %c[lastDecType]\n\t"
		".set DE_lastDec, %c[lastDec]\n"
		:: [numVerts] "i"(offsetof(DrawEngineCommon, numDrawVerts_)), [numInds] "i"(offsetof(DrawEngineCommon, numDrawInds_)),
		[inCalls] "i"(offsetof(DrawEngineCommon, vertexCountInDrawCalls_)), [verts] "i"(offsetof(DrawEngineCommon, drawVerts_)),
		[inds] "i"(offsetof(DrawEngineCommon, drawInds_)), [prevPrim] "i"(offsetof(DrawEngineCommon, prevPrim_)),
		[dec] "i"(offsetof(DrawEngineCommon, dec_)), [lastVType] "i"(offsetof(DrawEngineCommon, lastVType_)),
		[seen] "i"(offsetof(DrawEngineCommon, seenPrims_)), [anyCCW] "i"(offsetof(DrawEngineCommon, anyCCWOrIndexed_)),
		[bInds] "i"(offsetof(DrawEngineCommon, boundsInds_)), [bCount] "i"(offsetof(DrawEngineCommon, boundsCount_)),
		[bType] "i"(offsetof(DrawEngineCommon, boundsType_)), [bLower] "i"(offsetof(DrawEngineCommon, boundsLower_)),
		[bUpper] "i"(offsetof(DrawEngineCommon, boundsUpper_)), [lastDecType] "i"(offsetof(DrawEngineCommon, lastDecoderType_)),
		[lastDec] "i"(offsetof(DrawEngineCommon, lastDecoder_)));
	__asm__(
		".set DV_size, %c[dvSize]\n\t.set DV_verts, %c[dvVerts]\n\t.set DV_uv, %c[dvUv]\n\t.set DV_count, %c[dvCount]\n\t"
		".set DV_lower, %c[dvLower]\n\t.set DV_upper, %c[dvUpper]\n\t"
		".set DI_size, %c[diSize]\n\t.set DI_inds, %c[diInds]\n\t.set DI_count, %c[diCount]\n\t.set DI_decIndex, %c[diDec]\n\t"
		".set DI_indexType, %c[diType]\n\t.set DI_prim, %c[diPrim]\n\t.set DI_clockwise, %c[diCw]\n\t.set DI_offset, %c[diOffset]\n\t"
		".set VD_size, %c[vdSize]\n\t.set VD_skin, %c[vdSkin]\n\t"
		".set GSC_vaddr, %c[vaddr]\n\t.set GSC_iaddr, %c[iaddr]\n\t.set GSC_offset, %c[offset]\n\t.set GSC_uv, %c[uv]\n"
		:: [dvSize] "i"(sizeof(DrawEngineCommon::DeferredVerts)), [dvVerts] "i"(offsetof(DrawEngineCommon::DeferredVerts, verts)), [dvUv] "i"(offsetof(DrawEngineCommon::DeferredVerts, uvScale)), [dvCount] "i"(offsetof(DrawEngineCommon::DeferredVerts, vertexCount)),
		[dvLower] "i"(offsetof(DrawEngineCommon::DeferredVerts, indexLowerBound)), [dvUpper] "i"(offsetof(DrawEngineCommon::DeferredVerts, indexUpperBound)),
		[diSize] "i"(sizeof(DrawEngineCommon::DeferredInds)), [diInds] "i"(offsetof(DrawEngineCommon::DeferredInds, inds)), [diCount] "i"(offsetof(DrawEngineCommon::DeferredInds, vertexCount)), [diDec] "i"(offsetof(DrawEngineCommon::DeferredInds, vertDecodeIndex)),
		[diType] "i"(offsetof(DrawEngineCommon::DeferredInds, indexType)), [diPrim] "i"(offsetof(DrawEngineCommon::DeferredInds, prim)), [diCw] "i"(offsetof(DrawEngineCommon::DeferredInds, clockwise)), [diOffset] "i"(offsetof(DrawEngineCommon::DeferredInds, offset)),
		[vdSize] "i"(offsetof(VertexDecoder, size)), [vdSkin] "i"(offsetof(VertexDecoder, skinInDecode)),
		[vaddr] "i"(offsetof(GPUStateCache, vertexAddr)), [iaddr] "i"(offsetof(GPUStateCache, indexAddr)),
		[offset] "i"(offsetof(GPUStateCache, offsetAddr)), [uv] "i"(offsetof(GPUStateCache, uv)));
	__asm__(
		".set CMD_VADDR, %c[cVaddr]\n\t.set CMD_IADDR, %c[cIaddr]\n\t.set CMD_BASE, %c[cBase]\n\t.set CMD_OFFSETADDR, %c[cOffset]\n\t"
		".set CMD_CULLFACEENABLE, %c[cCullEn]\n\t.set CMD_CULL, %c[cCull]\n\t.set CMD_TEXSCALEU, %c[cScaleU]\n\t.set CMD_TEXLEVEL, %c[cLevel]\n\t"
		".set VT_IDX, %c[vtIdx]\n\t.set VT_CULLPASS, %c[vtPass]\n"
		// as int: an 8-bit operand is printed signed
		:: [cVaddr] "i"((int)GE_CMD_VADDR), [cIaddr] "i"((int)GE_CMD_IADDR), [cBase] "i"((int)GE_CMD_BASE), [cOffset] "i"((int)GE_CMD_OFFSETADDR),
		[cCullEn] "i"((int)GE_CMD_CULLFACEENABLE), [cCull] "i"((int)GE_CMD_CULL), [cScaleU] "i"((int)GE_CMD_TEXSCALEU), [cLevel] "i"((int)GE_CMD_TEXLEVEL),
		[vtIdx] "i"(GE_VTYPE_IDX_MASK),
		// PASSES_CULLING's vertex-type half: these draws aren't tested (Execute_Prim)
		[vtPass] "i"(GE_VTYPE_THROUGH_MASK | GE_VTYPE_MORPHCOUNT_MASK | GE_VTYPE_WEIGHT_MASK | GE_VTYPE_IDX_MASK));
	__asm__(
		"	stp	x29, x30, [sp, #-128]!\n"
		"	mov	x29, sp\n"
		"	stp	x19, x20, [sp, #16]\n"
		"	stp	x21, x22, [sp, #32]\n"
		"	stp	x23, x24, [sp, #48]\n"
		"	stp	x25, x26, [sp, #64]\n"
		"	stp	x27, x28, [sp, #80]\n"
		"	mov	x19, x1\n"
		"	str	x0, [sp, #112]\n"
		"	ldr	x23, [x19, #PL_engine]\n"
		"	ldr	x20, [x19, #PL_cmdmem]\n"
		"	ldr	x21, [x19, #PL_src]\n"
		"	ldr	x22, [x19, #PL_stall]\n"
		"	ldr	x24, [x19, #PL_gsc]\n"
		"	add	x25, x23, #(DE_dec & 0xFFF)\n"           // the draw engine's batch fields, past its arrays: from dec_
		"	add	x25, x25, #(DE_dec >> 12), lsl #12\n"
		"	ldr	x26, [x19, #PL_decoder]\n"
		"	ldr	w27, [x19, #PL_vtype]\n"
		"	ldr	w28, [x19, #PL_vtid]\n"
		".Lpl_loop:\n"
		"	cmp	x21, x22\n"
		"	b.eq	.Lpl_out\n"
		"	ldr	w9, [x21]\n"                       // the command
		"	lsr	w10, w9, #24\n"
		"	adrp	x11, GePrimLoopCases\n"
		"	add	x11, x11, :lo12:GePrimLoopCases\n"
		"	ldrb	w11, [x11, x10]\n"
		"	adr	x12, .Lpl_cases\n"
		"	add	x12, x12, x11, lsl #2\n"
		"	br	x12\n"
		".Lpl_cases:\n"
		"	b	.Lpl_bail\n"                       // 0: any other command: the lookahead ends
		"	b	.Lpl_prim\n"                       // 1
		"	b	.Lpl_vtype\n"                      // 2
		"	b	.Lpl_vaddr\n"                      // 3
		"	b	.Lpl_iaddr\n"                      // 4
		"	b	.Lpl_offset\n"                     // 5
		"	b	.Lpl_store\n"                      // 6: BASE, TEXFLUSH, NOP, NOP_FF, BONEMATRIXNUMBER: only the word kept
		"	b	.Lpl_cullenable\n"                 // 7
		"	b	.Lpl_cull\n"                       // 8
		"	b	.Lpl_uv\n"                         // 9: TEXSCALEU/V, TEXOFFSETU/V
		"	b	.Lpl_texlevel\n"                   // 10
		"	b	.Lpl_call\n"                       // 11: CALL: a bone matrix, or the lookahead's end
		"	b	.Lpl_same\n"                       // 12: TEXBUFWIDTH0, TEXADDR0: on only while they don't change
		".Lpl_next:\n"
		"	add	x21, x21, #4\n"
		"	b	.Lpl_loop\n"

		".Lpl_store:\n"
		"	str	w9, [x20, x10, lsl #2]\n"
		"	b	.Lpl_next\n"

		// the address: gstate_c.getRelativeAddress(data & 0xFFFFFF), in w12
		".Lpl_vaddr:\n"
		"	str	w9, [x20, #(CMD_VADDR * 4)]\n"
		"	ldr	w12, [x20, #(CMD_BASE * 4)]\n"
		"	and	w12, w12, #0x000F0000\n"
		"	and	w13, w9, #0x00FFFFFF\n"
		"	orr	w12, w13, w12, lsl #8\n"
		"	ldr	w13, [x24, #GSC_offset]\n"
		"	add	w12, w12, w13\n"
		"	and	w12, w12, #0x0FFFFFFF\n"
		"	ldr	w13, [x24, #GSC_vaddr]\n"
		"	cmp	w12, w13\n"
		"	b.eq	.Lpl_next\n"
		"	strb	wzr, [x19, #PL_canExtend]\n"
		"	str	w12, [x24, #GSC_vaddr]\n"
		"	b	.Lpl_next\n"
		".Lpl_iaddr:\n"
		"	str	w9, [x20, #(CMD_IADDR * 4)]\n"
		"	ldr	w12, [x20, #(CMD_BASE * 4)]\n"
		"	and	w12, w12, #0x000F0000\n"
		"	and	w13, w9, #0x00FFFFFF\n"
		"	orr	w12, w13, w12, lsl #8\n"
		"	ldr	w13, [x24, #GSC_offset]\n"
		"	add	w12, w12, w13\n"
		"	and	w12, w12, #0x0FFFFFFF\n"
		"	str	w12, [x24, #GSC_iaddr]\n"
		"	b	.Lpl_next\n"
		".Lpl_offset:\n"
		"	str	w9, [x20, #(CMD_OFFSETADDR * 4)]\n"
		"	lsl	w12, w9, #8\n"
		"	str	w12, [x24, #GSC_offset]\n"
		"	b	.Lpl_next\n"
		".Lpl_cullenable:\n"                       // a change ends it
		"	ldr	w12, [x20, #(CMD_CULLFACEENABLE * 4)]\n"
		"	cmp	w12, w9\n"
		"	b.ne	.Lpl_bail\n"
		"	b	.Lpl_next\n"
		".Lpl_cull:\n"                             // the winding the rest is drawn with
		"	and	w12, w9, #1\n"
		"	str	w12, [x19, #PL_cullMode]\n"
		"	b	.Lpl_next\n"
		".Lpl_uv:\n"                               // gstate_c.uv's field = getFloat24(data); no more extending
		"	str	w9, [x20, x10, lsl #2]\n"
		"	sub	w12, w10, #CMD_TEXSCALEU\n"
		"	lsl	w13, w9, #8\n"
		"	add	x14, x24, #GSC_uv\n"
		"	str	w13, [x14, x12, lsl #2]\n"
		"	strb	wzr, [x19, #PL_canExtend]\n"
		"	b	.Lpl_next\n"
		".Lpl_texlevel:\n"                         // Execute_TexLevel's Gran Turismo case ends it
		"	tst	w9, #3\n"
		"	b.eq	1f\n"
		"	tst	w9, #0x00FF0000\n"
		"	b.ne	.Lpl_bail\n"
		"1:	str	w9, [x20, #(CMD_TEXLEVEL * 4)]\n"
		"	b	.Lpl_next\n"
		".Lpl_same:\n"
		"	ldr	w12, [x20, x10, lsl #2]\n"
		"	cmp	w12, w9\n"
		"	b.ne	.Lpl_bail\n"
		"	b	.Lpl_next\n"

		".Lpl_call:\n"
		"	ldr	x0, [sp, #112]\n"
		"	mov	w1, w9\n"
		"	bl	GePrimBoneCall\n"
		"	tst	w0, #0xFF\n"
		"	b.eq	.Lpl_bail\n"
		"	strb	wzr, [x19, #PL_canExtend]\n"
		"	b	.Lpl_next\n"

		// VERTEXTYPE: only the weight count may change (vtypeCheckMask); the new decoder must be GetVertexDecoder's last one
		".Lpl_vtype:\n"
		"	eor	w12, w9, w27\n"
		"	cbz	w12, .Lpl_next\n"
		"	ldr	w13, [x19, #PL_vtmask]\n"
		"	tst	w12, w13\n"
		"	b.ne	.Lpl_bail\n"
		"	and	w14, w9, #0x00FFFFFF\n"
		"	ldr	w13, [x19, #PL_idHigh]\n"
		"	orr	w14, w14, w13\n"
		"	ldr	w13, [x23, #DE_lastDecType]\n"
		"	cmp	w13, w14\n"
		"	b.ne	.Lpl_out\n"
		"	mov	w27, w9\n"
		"	mov	w28, w14\n"
		"	ldr	x26, [x23, #DE_lastDec]\n"
		"	ldr	x0, [x25, #(DE_dec - DE_dec)]\n"              // FlushSkin: what's queued is skinned with the matrices it had
		"	cbz	x0, 1f\n"
		"	ldrb	w1, [x0, #VD_skin]\n"
		"	cbz	w1, 1f\n"
		"	mov	x0, x23\n"
		"	bl	GePrimFlushSkin\n"
		"1:	strb	wzr, [x19, #PL_canExtend]\n"
		"	b	.Lpl_next\n"

		// PRIM: w11 its type, w14 its count, w15 clockwise, w16 the vertex address
		".Lpl_prim:\n"
		"	ubfx	w11, w9, #16, #3\n"
		"	cmp	w11, #2\n"
		"	ccmp	w11, #6, #4, hi\n"
		"	cset	w12, ne\n"                         // IsTrianglePrim
		"	ldrb	w13, [x19, #PL_isTriangle]\n"
		"	cmp	w12, w13\n"
		"	b.ne	.Lpl_bail\n"
		"	and	w14, w9, #0xFFFF\n"
		"	ldr	w12, [x20, #(CMD_CULLFACEENABLE * 4)]\n"
		"	ldr	w13, [x20, #(CMD_CULL * 4)]\n"
		"	ldr	w15, [x19, #PL_cullMode]\n"
		"	and	w13, w13, #1\n"
		"	cmp	w13, w15\n"
		"	cset	w15, eq\n"
		"	tst	w12, #1\n"
		"	csinc	w15, w15, wzr, ne\n"               // clockwise: culling off, or still the winding it began with
		"	ldrb	w12, [x19, #PL_canExtend]\n"
		"	cbnz	w12, .Lpl_extend\n"

		// a draw of its own: first every case Execute_Prim takes, before anything changes
		"	tst	w27, #VT_IDX\n"
		"	b.ne	.Lpl_out\n"                        // indexed
		"	ldr	w16, [x24, #GSC_vaddr]\n"
		"1:	cmp	w11, #6\n"
		"	b.hs	.Lpl_out\n"                        // rectangles (a self-render's flush), KEEP_PREVIOUS
		"	cmp	w14, #3\n"
		"	b.lo	.Lpl_out\n"                        // too few vertices for SubmitPrim
		"	ldrsb	w12, [x25, #(DE_prevPrim - DE_dec)]\n"        // PrimCompatible(prevPrim_, prim), else a flush
		"	cmn	w12, #1\n"
		"	b.eq	2f\n"
		"	cmp	w12, #6\n"
		"	b.hi	.Lpl_out\n"
		"	mov	x13, #0x0100\n"                    // indexedPrimitiveType: byte p is p's class
		"	movk	x13, #0x0301, lsl #16\n"
		"	movk	x13, #0x0303, lsl #32\n"
		"	movk	x13, #0x0006, lsl #48\n"
		"	lsl	w12, w12, #3\n"
		"	lsr	x12, x13, x12\n"
		"	lsl	w17, w11, #3\n"
		"	lsr	x17, x13, x17\n"
		"	eor	w12, w12, w17\n"
		"	tst	w12, #0xFF\n"
		"	b.ne	.Lpl_out\n"
		"2:	ldr	w12, [x25, #(DE_numVerts - DE_dec)]\n"        // room, else a flush
		"	cmp	w12, #128\n"
		"	b.ge	.Lpl_out\n"
		"	ldr	w12, [x25, #(DE_numInds - DE_dec)]\n"
		"	cmp	w12, #512\n"
		"	b.ge	.Lpl_out\n"
		"	ldr	w12, [x25, #(DE_inCalls - DE_dec)]\n"
		"	add	w12, w12, w14\n"
		"	cmp	w12, #16, lsl #12\n"
		"	b.gt	.Lpl_out\n"
		// it's this loop's
		"	ldrb	w12, [x19, #PL_isTriangle]\n"
		"	strb	w12, [x19, #PL_canExtend]\n"
		"	mov	w17, w16\n"                       // ReadThroughUploads: vertices just copied into VRAM are read where they came from
		"	ldrb	w12, [x19, #PL_vramPending]\n"
		"	cbz	w12, 1f\n"
		"	and	w12, w16, #0x3F800000\n"
		"	mov	w13, #0x04000000\n"
		"	cmp	w12, w13\n"
		"	b.ne	1f\n"
		"	stp	w11, w14, [sp, #96]\n"
		"	stp	w15, w16, [sp, #104]\n"
		"	mov	w0, w16\n"
		"	ldrh	w1, [x26, #VD_size]\n"
		"	mul	w1, w1, w14\n"
		"	bl	GePrimReadable\n"
		"	mov	w17, w0\n"
		"	ldp	w11, w14, [sp, #96]\n"
		"	ldp	w15, w16, [sp, #104]\n"
		"1:\n"
		"	ldrb	w12, [x19, #PL_onePassed]\n"
		"	cbnz	w12, .Lpl_submit\n"
		"	mov	w12, #(VT_CULLPASS & 0xFFFF)\n"
		"	movk	w12, #(VT_CULLPASS >> 16), lsl #16\n"
		"	tst	w27, w12\n"
		"	b.ne	.Lpl_submit\n"
		"	cmp	w14, #6\n"
		"	b.hi	.Lpl_submit\n"
		"	stp	w11, w14, [sp, #96]\n"
		"	stp	w15, w16, [sp, #104]\n"
		"	str	w17, [sp, #120]\n"
		"	mov	x0, x23\n"
		"	ldr	x1, [x19, #PL_base]\n"
		"	add	x1, x1, x17\n"
		"	mov	w2, w14\n"
		"	mov	x3, x26\n"
		"	mov	w4, w27\n"
		"	bl	GePrimCullTest\n"
		"	ldp	w11, w14, [sp, #96]\n"
		"	ldp	w15, w16, [sp, #104]\n"
		"	ldr	w17, [sp, #120]\n"
		"	tst	w0, #0xFF\n"
		"	b.ne	.Lpl_submit\n"
		// culled: counted, and SkipPrim (prevPrim_; nothing queued); no more extending
		"	ldr	x12, [x19, #PL_culled]\n"
		"	ldr	w13, [x12]\n"
		"	add	w13, w13, #1\n"
		"	str	w13, [x12]\n"
		"	strb	w11, [x25, #(DE_prevPrim - DE_dec)]\n"
		"	strb	wzr, [x19, #PL_canExtend]\n"
		"	b	.Lpl_advance\n"

		// SubmitPrim, its path for a non-indexed draw that fits and needs no flush
		".Lpl_submit:\n"
		"	strb	w11, [x25, #(DE_prevPrim - DE_dec)]\n"
		"	ldr	w12, [x25, #(DE_lastVType - DE_dec)]\n"
		"	cmp	w12, w28\n"
		"	b.eq	1f\n"
		"	str	x26, [x25, #(DE_dec - DE_dec)]\n"
		"	str	w28, [x25, #(DE_lastVType - DE_dec)]\n"
		"1:	mov	w7, w14\n"                         // the count drawn: triangles in threes
		"	cmp	w11, #3\n"
		"	b.ne	2f\n"
		"	mov	w12, #3\n"
		"	udiv	w13, w14, w12\n"
		"	mul	w7, w13, w12\n"
		"2:	ldr	w12, [x25, #(DE_numInds - DE_dec)]\n"
		"	add	w13, w12, #1\n"
		"	str	w13, [x25, #(DE_numInds - DE_dec)]\n"
		"	mov	x13, #DI_size\n"
		"	umaddl	x13, w12, w13, x23\n"
		"	add	x13, x13, #(DE_inds & 0xFFF)\n"
		"	add	x13, x13, #(DE_inds >> 12), lsl #12\n"
		"	str	xzr, [x13, #DI_inds]\n"
		"	str	w7, [x13, #DI_count]\n"
		"	ldr	w12, [x25, #(DE_numVerts - DE_dec)]\n"
		"	strb	w12, [x13, #DI_decIndex]\n"
		"	strb	wzr, [x13, #DI_indexType]\n"
		"	strb	w11, [x13, #DI_prim]\n"
		"	strb	w15, [x13, #DI_clockwise]\n"
		"	strh	wzr, [x13, #DI_offset]\n"
		"	cbnz	w15, 3f\n"
		"	mov	w13, #1\n"
		"	strb	w13, [x25, #(DE_anyCCW - DE_dec)]\n"
		"3:	add	w13, w12, #1\n"
		"	str	w13, [x25, #(DE_numVerts - DE_dec)]\n"
		"	mov	x13, #DV_size\n"
		"	umaddl	x13, w12, w13, x23\n"
		"	add	x13, x13, #DE_verts\n"
		"	ldr	x12, [x19, #PL_base]\n"
		"	add	x12, x12, x17\n"
		"	str	x12, [x13, #DV_verts]\n"
		"	str	w7, [x13, #DV_count]\n"
		"	ldr	q0, [x24, #GSC_uv]\n"
		"	str	q0, [x13, #DV_uv]\n"
		"	sub	w12, w7, #1\n"                     // IndexBounds(nullptr, count): 0 to count - 1, and its cache's fields
		"	strh	wzr, [x13, #DV_lower]\n"
		"	strh	w12, [x13, #DV_upper]\n"
		"	str	xzr, [x25, #(DE_bInds - DE_dec)]\n"
		"	str	w7, [x25, #(DE_bCount - DE_dec)]\n"
		"	str	w28, [x25, #(DE_bType - DE_dec)]\n"
		"	strh	wzr, [x25, #(DE_bLower - DE_dec)]\n"
		"	strh	w12, [x25, #(DE_bUpper - DE_dec)]\n"
		"	ldr	x12, [x19, #PL_base]\n"
		"	add	x12, x12, x17\n"                   // the vertices' first lines on their way: they're decoded at the flush
		"	prfm	pldl1keep, [x12]\n"
		"	prfm	pldl1keep, [x12, #64]\n"
		"	prfm	pldl1keep, [x12, #128]\n"
		"	ldr	w12, [x25, #(DE_inCalls - DE_dec)]\n"
		"	add	w12, w12, w7\n"
		"	str	w12, [x25, #(DE_inCalls - DE_dec)]\n"
		"	ldr	w12, [x25, #(DE_seen - DE_dec)]\n"
		"	mov	w13, #1\n"
		"	lsl	w13, w13, w11\n"
		"	orr	w12, w12, w13\n"
		"	str	w12, [x25, #(DE_seen - DE_dec)]\n"
		"	mov	w12, #1\n"
		"	strb	w12, [x19, #PL_onePassed]\n"
		// AdvanceVerts: bytesRead = count * the vertex's size; the totals
		".Lpl_advance:\n"
		"	ldrh	w12, [x26, #VD_size]\n"
		"	mul	w12, w12, w14\n"
		"	str	w12, [x19, #PL_bytesRead]\n"
		"	add	w16, w16, w12\n"
		"	str	w16, [x24, #GSC_vaddr]\n"
		"	ldr	w13, [x19, #PL_total]\n"
		"	add	w13, w13, w14\n"
		"	str	w13, [x19, #PL_total]\n"
		"	b	.Lpl_next\n"

		// Merging: the PRIMs from here, while they're PRIMs of this kind and fit, onto the last draw's vertices.
		// x13 that draw, w16 the offset (its vertices so far), w17 its count before, x8 the next record, w6 a record's word at
		// DI_decIndex (the draw, index type 0, the type, the winding), x2 where the records or the commands end, w4 the vertices
		// that still fit, w7 the types of this kind (a bit each), w0 the types seen, x5 the command
		".Lpl_extend:\n"
		"	ldr	w12, [x25, #(DE_numVerts - DE_dec)]\n"
		"	sub	w12, w12, #1\n"
		"	mov	x13, #DV_size\n"
		"	umaddl	x13, w12, w13, x23\n"
		"	add	x13, x13, #DE_verts\n"
		"	ldr	w16, [x13, #DV_count]\n"
		"	mov	w17, w16\n"
		"	cbnz	w15, 1f\n"
		"	mov	w0, #1\n"
		"	strb	w0, [x25, #(DE_anyCCW - DE_dec)]\n"
		"1:	orr	w6, w12, w15, lsl #24\n"
		"	ldr	w1, [x25, #(DE_numInds - DE_dec)]\n"
		"	mov	x8, #DI_size\n"
		"	umaddl	x8, w1, w8, x23\n"
		"	add	x8, x8, #(DE_inds & 0xFFF)\n"
		"	add	x8, x8, #(DE_inds >> 12), lsl #12\n"
		"	mov	w2, #512\n"
		"	sub	w2, w2, w1\n"
		"	add	x2, x21, x2, lsl #2\n"            // the records run out at this command
		"	cmp	x22, x21\n"
		"	ccmp	x22, x2, #2, hi\n"
		"	csel	x2, x22, x2, lo\n"                // or the stall first, if it's between (none: null, behind: never met)
		"	ldr	w3, [x25, #(DE_inCalls - DE_dec)]\n"
		"	mov	w4, #0x10000\n"
		"	sub	w4, w4, w3\n"
		"	sub	w4, w4, w16\n"
		"	ldrb	w7, [x19, #PL_isTriangle]\n"
		"	mov	w5, #0x47\n"                      // points, lines, line strips, rectangles
		"	mov	w0, #0xB8\n"                      // triangles, strips, fans and KEEP_PREVIOUS (IsTrianglePrim)
		"	cmp	w7, #0\n"
		"	csel	w7, w5, w0, eq\n"
		"	mov	w0, #0\n"
		"	mov	w14, #1\n"
		"	mov	x5, x21\n"
		"2:	cmp	x5, x2\n"
		"	b.eq	3f\n"
		"	ldr	w9, [x5]\n"
		"	lsr	w10, w9, #19\n"
		"	cmp	w10, #0x80\n"                     // a PRIM
		"	b.ne	3f\n"
		"	ubfx	w10, w9, #16, #3\n"
		"	lsr	w11, w7, w10\n"
		"	tbz	w11, #0, 3f\n"                    // of this kind
		"	and	w11, w9, #0xFFFF\n"
		"	subs	w4, w4, w11\n"
		"	b.lt	3f\n"                             // that fits
		"	bfi	w6, w10, #16, #8\n"
		"	str	w11, [x8, #DI_count]\n"
		"	str	w6, [x8, #DI_decIndex]\n"
		"	strh	w16, [x8, #DI_offset]\n"
		"	add	x8, x8, #DI_size\n"
		"	add	w16, w16, w11\n"
		"	lsl	w11, w14, w10\n"
		"	orr	w0, w0, w11\n"
		"	add	x5, x5, #4\n"
		"	b	2b\n"
		"3:	sub	x9, x5, x21\n"
		"	add	w1, w1, w9, lsr #2\n"
		"	str	w1, [x25, #(DE_numInds - DE_dec)]\n"
		"	ldr	w9, [x25, #(DE_seen - DE_dec)]\n"
		"	orr	w9, w9, w0\n"
		"	str	w9, [x25, #(DE_seen - DE_dec)]\n"
		"	sub	w9, w16, w17\n"                   // the vertices added
		"	str	w16, [x13, #DV_count]\n"
		"	sub	w6, w16, #1\n"
		"	strh	w6, [x13, #DV_upper]\n"
		"	add	w3, w3, w9\n"
		"	str	w3, [x25, #(DE_inCalls - DE_dec)]\n"
		"	ldrh	w6, [x26, #VD_size]\n"
		"	mul	w6, w6, w9\n"
		"	str	w6, [x19, #PL_bytesRead]\n"
		"	cmp	x5, x21\n"
		"	b.eq	.Lpl_bail\n"                      // not even this one: the lookahead ends
		"	sub	x21, x5, #4\n"
		"	ldr	w7, [x24, #GSC_vaddr]\n"
		"	add	w7, w7, w6\n"
		"	str	w7, [x24, #GSC_vaddr]\n"
		"	ldr	w7, [x19, #PL_total]\n"            // every PRIM taken, each charged as if run alone
		"	add	w7, w7, w9\n"
		"	str	w7, [x19, #PL_total]\n"
		"	b	.Lpl_next\n"

		".Lpl_bail:\n"
		"	mov	w9, #1\n"
		"	b	1f\n"
		".Lpl_out:\n"
		"	mov	w9, #0\n"
		"1:	strb	w9, [x19, #PL_bail]\n"
		"	str	x21, [x19, #PL_src]\n"
		"	str	x26, [x19, #PL_decoder]\n"
		"	str	w27, [x19, #PL_vtype]\n"
		"	str	w28, [x19, #PL_vtid]\n"
		"	ldp	x19, x20, [sp, #16]\n"
		"	ldp	x21, x22, [sp, #32]\n"
		"	ldp	x23, x24, [sp, #48]\n"
		"	ldp	x25, x26, [sp, #64]\n"
		"	ldp	x27, x28, [sp, #80]\n"
		"	ldp	x29, x30, [sp], #128\n"
		"	ret\n");
}

#endif
