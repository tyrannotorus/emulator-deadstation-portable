// Deadstation Portable: vertex decoders in hand-written ARM64, for formats the JIT's step-by-step code decodes slowly.
//
// T: u16  N: s8  P: float (20 bytes in; 24 out: uv float2 at 0, normal s8 x3 + 0 at 8, position float3 at 12): OutRun 2006's
// most used format (13% of its GE thread as the JIT emits it). Two vertices a loop, their uv chains interleaved; the normal and
// position loaded as one 16-byte pair and stored as one; stores in address order (the destination is uncached Vulkan memory,
// write-combined); the source prefetched 320 bytes ahead. Raw cycles a vertex on the A53 (local/brick/vdecbench), destination
// in the Vulkan memory the emulator decodes into: source in L1 13.9 against the JIT's 33.2, source streamed from RAM 30.1 / 38.6.
//   VdecHand_Tu16pre_Ns8_Pf: uv prescaled (uv * scale + offset, scale and offset from x3), the usual case
//   VdecHand_Tu16_Ns8_Pf: uv / 32768 only (texture matrix uv generation)
// void (const u8 *src, u8 *dst, int count, const UVScale *uvScaleOffset), as JittedVertexDecoder.
#if defined(__aarch64__)
#define VDEC_TU16_NS8_PF(NAME, PRESCALE_SETUP, UV_CVT, UV_FIN, UV_1) \
__asm__( \
	".text\n" \
	".global " NAME "\n" \
	".type " NAME ", %function\n" \
	".p2align 4\n" \
	NAME ":\n" \
	PRESCALE_SETUP \
	"	mov	x9, #0xffffffff00ffffff\n"  /* the normal's 4th byte cleared */ \
	"	subs	w2, w2, #2\n" \
	"	b.lt	2f\n" \
	"	.p2align 4\n" \
	"1:\n" \
	"	add	x12, x0, #4\n" \
	"	add	x13, x0, #24\n" \
	"	ldr	s3, [x0]\n" \
	"	ldr	s4, [x0, #20]\n" \
	"	ldp	x6, x7, [x12]\n"             /* normal | pos.x, pos.y | pos.z */ \
	"	ldp	x10, x11, [x13]\n" \
	"	prfm	pldl1keep, [x0, #320]\n" \
	"	ushll	v3.4s, v3.4h, #0\n" \
	"	ushll	v4.4s, v4.4h, #0\n" \
	"	and	x6, x6, x9\n" \
	"	and	x10, x10, x9\n" \
	UV_CVT \
	"	add	x0, x0, #40\n" \
	UV_FIN \
	"	subs	w2, w2, #2\n" \
	"	str	d2, [x1]\n"                  /* in address order */ \
	"	stp	x6, x7, [x1, #8]\n" \
	"	str	d5, [x1, #24]\n" \
	"	stp	x10, x11, [x1, #32]\n" \
	"	add	x1, x1, #48\n" \
	"	b.ge	1b\n" \
	"2:\n" \
	"	adds	w2, w2, #2\n"                /* 0 or 1 left */ \
	"	b.eq	3f\n" \
	"	ldr	s3, [x0]\n" \
	"	ldur	x6, [x0, #4]\n" \
	"	ldur	x7, [x0, #12]\n" \
	"	ushll	v3.4s, v3.4h, #0\n" \
	"	and	x6, x6, x9\n" \
	UV_1 \
	"	str	d2, [x1]\n" \
	"	stp	x6, x7, [x1, #8]\n" \
	"3:\n" \
	"	ret\n" \
	".size " NAME ", .-" NAME "\n" \
);

VDEC_TU16_NS8_PF("VdecHand_Tu16pre_Ns8_Pf",
	"	ldp	d0, d1, [x3]\n",                     /* uv scale, offset */
	"	ucvtf	v3.2s, v3.2s, #15\n"                  /* both converted, then both scaled */
	"	ucvtf	v4.2s, v4.2s, #15\n"
	"	mov	v2.8b, v1.8b\n"
	"	mov	v5.8b, v1.8b\n",
	"	fmla	v2.2s, v3.2s, v0.2s\n"
	"	fmla	v5.2s, v4.2s, v0.2s\n",
	"	ucvtf	v3.2s, v3.2s, #15\n"
	"	mov	v2.8b, v1.8b\n"
	"	fmla	v2.2s, v3.2s, v0.2s\n")

VDEC_TU16_NS8_PF("VdecHand_Tu16_Ns8_Pf",
	"",
	"	ucvtf	v2.2s, v3.2s, #15\n"
	"	ucvtf	v5.2s, v4.2s, #15\n",
	"",
	"	ucvtf	v2.2s, v3.2s, #15\n")
// T: u16 (prescaled)  N: s8 (4 bytes as they are)  C: 8888  P: float (24 bytes in; 28 out: uv float2, normal, color, position):
// God of War's, as VtxDec_Tu16_C8888_Pfloat in C++ (the same operations: uv converted, * (scale / 32768), + offset, not fused; alpha
// the AND of every color + position vector's first lane). Two vertices a loop, their uv chains interleaved, stores in address
// order (the compiled C++ stored color + position, then normal, then uv), the normal moved by an integer register, the source
// prefetched. Returns 1 when every alpha is 0xFF.
// int VdecHand_Tu16pre_N8_C8888_Pf(const u8 *src, u8 *dst, int count, const UVScale *uvScaleOffset)
__asm__(
	".text\n"
	".global VdecHand_Tu16pre_N8_C8888_Pf\n"
	".type VdecHand_Tu16pre_N8_C8888_Pf, %function\n"
	".p2align 4\n"
	"VdecHand_Tu16pre_N8_C8888_Pf:\n"
	"	ldp	d2, d0, [x3]\n"              /* uv scale, offset */
	"	mov	w9, #0x38000000\n"           /* 1 / 32768 */
	"	dup	v1.2s, w9\n"
	"	fmul	v2.2s, v2.2s, v1.2s\n"
	"	movi	v1.2d, #0xffffffffffffffff\n" /* the alpha AND */
	"	subs	w2, w2, #2\n"
	"	b.lt	2f\n"
	"	.p2align 4\n"
	"1:\n"
	"	ldur	d3, [x0]\n"                  /* A: u, v, normal */
	"	ldur	q5, [x0, #8]\n"              /* A: color, x, y, z */
	"	ldur	d4, [x0, #24]\n"             /* B */
	"	ldur	q6, [x0, #32]\n"
	"	prfm	pldl1keep, [x0, #384]\n"
	"	ushll	v16.4s, v3.4h, #0\n"
	"	ushll	v17.4s, v4.4h, #0\n"
	"	umov	w9, v3.s[1]\n"
	"	umov	w10, v4.s[1]\n"
	"	ucvtf	v16.2s, v16.2s\n"
	"	ucvtf	v17.2s, v17.2s\n"
	"	and	v1.16b, v5.16b, v1.16b\n"
	"	and	v1.16b, v6.16b, v1.16b\n"
	"	fmul	v16.2s, v2.2s, v16.2s\n"
	"	fmul	v17.2s, v2.2s, v17.2s\n"
	"	add	x0, x0, #48\n"
	"	subs	w2, w2, #2\n"
	"	fadd	v16.2s, v0.2s, v16.2s\n"
	"	fadd	v17.2s, v0.2s, v17.2s\n"
	"	str	d16, [x1]\n"                 /* in address order */
	"	str	w9, [x1, #8]\n"
	"	stur	q5, [x1, #12]\n"
	"	stur	d17, [x1, #28]\n"
	"	str	w10, [x1, #36]\n"
	"	stur	q6, [x1, #40]\n"
	"	add	x1, x1, #56\n"
	"	b.ge	1b\n"
	"2:\n"
	"	adds	w2, w2, #2\n"
	"	b.eq	3f\n"
	"	ldur	d3, [x0]\n"
	"	ldur	q5, [x0, #8]\n"
	"	ushll	v16.4s, v3.4h, #0\n"
	"	umov	w9, v3.s[1]\n"
	"	ucvtf	v16.2s, v16.2s\n"
	"	and	v1.16b, v5.16b, v1.16b\n"
	"	fmul	v16.2s, v2.2s, v16.2s\n"
	"	fadd	v16.2s, v0.2s, v16.2s\n"
	"	str	d16, [x1]\n"
	"	str	w9, [x1, #8]\n"
	"	stur	q5, [x1, #12]\n"
	"3:\n"
	"	umov	w0, v1.s[0]\n"
	"	lsr	w0, w0, #24\n"
	"	cmp	w0, #0xff\n"
	"	cset	w0, eq\n"
	"	ret\n"
	".size VdecHand_Tu16pre_N8_C8888_Pf, .-VdecHand_Tu16pre_N8_C8888_Pf\n"
);
// W: float (1x, skinned)  T: u8 (prescaled)  C: 4444  N: s8  P: float (24 bytes in; 36 out: uv float2 at 0, color at 8, normal
// float3 at 12, position float3 at 24): God of War's most used skinned format. The JIT's own operations in its own order for each
// value (the bone matrix rows masked to 3 lanes; matrix = bone * weight; normal = m0 * n.x, + m1 * n.y, + m2 * n.z; position the
// same, + m3; uv = offset + uv * scale, fused; 4444 expanded as the JIT does), two vertices a loop: their blends, conversions and
// four transform chains interleaved (one vertex at a time, the A53 waited on each chain), stores in address order as the JIT's
// (the 16-byte normal and position stores included). Returns 0 when some alpha isn't 0xF (the caller clears vertexFullAlpha).
// int VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf(const u8 *src, u8 *dst, int count, const UVScale *uvScaleOffset, const float *boneMatrix)
// v0 uv scale, v1 uv offset, v16-v19 the bone's rows, v20-v23 / v24-v27 the two matrices, v28-v31 the four accumulators,
// v8 / v9 the two positions (d8, d9 saved), w15 the alpha-not-full bits.
__asm__(
	".text\n"
	".global VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf\n"
	".type VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf, %function\n"
	".p2align 4\n"
	"VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf:\n"
	"	stp	d8, d9, [sp, #-16]!\n"
	"	ldp	d0, d1, [x3]\n"
	/* the bone's 3x4 floats as four rows of three, the fourth lane zeroed (as the JIT) */
	"	mvni	v3.4s, #0\n"
	"	movi	v4.4s, #0\n"
	"	ext	v3.16b, v3.16b, v4.16b, #4\n"
	"	ld1	{ v4.4s, v5.4s, v6.4s }, [x4]\n"
	"	ext	v7.16b, v6.16b, v6.16b, #4\n"
	"	ext	v6.16b, v5.16b, v6.16b, #8\n"
	"	ext	v5.16b, v4.16b, v5.16b, #12\n"
	"	and	v16.16b, v4.16b, v3.16b\n"
	"	and	v17.16b, v5.16b, v3.16b\n"
	"	and	v18.16b, v6.16b, v3.16b\n"
	"	and	v19.16b, v7.16b, v3.16b\n"
	"	mov	w15, #0\n"
	"	subs	w2, w2, #2\n"
	"	b.lt	2f\n"
	"	.p2align 4\n"
	"1:\n"
	"	ldr	s4, [x0]\n"                    /* weights */
	"	ldr	s5, [x0, #24]\n"
	"	ldur	s6, [x0, #8]\n"                /* normals */
	"	ldur	s7, [x0, #32]\n"
	"	ldur	q8, [x0, #12]\n"               /* positions */
	"	ldur	q9, [x0, #36]\n"
	"	ldur	h2, [x0, #4]\n"                /* uv */
	"	ldur	h3, [x0, #28]\n"
	"	ldrh	w6, [x0, #6]\n"                /* colors */
	"	ldrh	w12, [x0, #30]\n"
	"	fmul	v20.4s, v16.4s, v4.s[0]\n"     /* the two matrices */
	"	fmul	v24.4s, v16.4s, v5.s[0]\n"
	"	and	w7, w6, #0xf\n"
	"	and	w8, w6, #0xf0\n"
	"	fmul	v21.4s, v17.4s, v4.s[0]\n"
	"	fmul	v25.4s, v17.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #4\n"
	"	and	w8, w6, #0xf00\n"
	"	fmul	v22.4s, v18.4s, v4.s[0]\n"
	"	fmul	v26.4s, v18.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #8\n"
	"	and	w8, w6, #0xf000\n"
	"	fmul	v23.4s, v19.4s, v4.s[0]\n"
	"	fmul	v27.4s, v19.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #12\n"
	"	and	w13, w12, #0xf\n"
	"	sshll	v6.8h, v6.8b, #0\n"            /* normals to float */
	"	sshll	v7.8h, v7.8b, #0\n"
	"	orr	w6, w7, w7, lsl #4\n"          /* color A done */
	"	and	w14, w12, #0xf0\n"
	"	sshll	v6.4s, v6.4h, #0\n"
	"	sshll	v7.4s, v7.4h, #0\n"
	"	orr	w13, w13, w14, lsl #4\n"
	"	and	w14, w12, #0xf00\n"
	"	scvtf	v6.4s, v6.4s, #7\n"
	"	scvtf	v7.4s, v7.4s, #7\n"
	"	orr	w13, w13, w14, lsl #8\n"
	"	and	w14, w12, #0xf000\n"
	"	ushll	v2.8h, v2.8b, #0\n"            /* uv to float */
	"	ushll	v3.8h, v3.8b, #0\n"
	"	orr	w13, w13, w14, lsl #12\n"
	"	orn	w15, w15, w6, asr #24\n"
	"	ushll	v2.4s, v2.4h, #0\n"
	"	ushll	v3.4s, v3.4h, #0\n"
	"	orr	w12, w13, w13, lsl #4\n"       /* color B done */
	"	ucvtf	v2.2s, v2.2s, #7\n"
	"	ucvtf	v3.2s, v3.2s, #7\n"
	"	orn	w15, w15, w12, asr #24\n"
	"	fmul	v28.4s, v20.4s, v6.s[0]\n"     /* four chains: normal A, position A, normal B, position B */
	"	fmul	v29.4s, v20.4s, v8.s[0]\n"
	"	fmul	v30.4s, v24.4s, v7.s[0]\n"
	"	fmul	v31.4s, v24.4s, v9.s[0]\n"
	"	mov	v4.8b, v1.8b\n"
	"	mov	v5.8b, v1.8b\n"
	"	fmla	v28.4s, v21.4s, v6.s[1]\n"
	"	fmla	v29.4s, v21.4s, v8.s[1]\n"
	"	fmla	v30.4s, v25.4s, v7.s[1]\n"
	"	fmla	v31.4s, v25.4s, v9.s[1]\n"
	"	fmla	v4.2s, v2.2s, v0.2s\n"
	"	fmla	v5.2s, v3.2s, v0.2s\n"
	"	fmla	v28.4s, v22.4s, v6.s[2]\n"
	"	fmla	v29.4s, v22.4s, v8.s[2]\n"
	"	fmla	v30.4s, v26.4s, v7.s[2]\n"
	"	fmla	v31.4s, v26.4s, v9.s[2]\n"
	"	add	x0, x0, #48\n"
	"	subs	w2, w2, #2\n"
	"	fadd	v29.4s, v29.4s, v23.4s\n"
	"	fadd	v31.4s, v31.4s, v27.4s\n"
	"	stur	d4, [x1]\n"                    /* A, then B, in address order */
	"	str	w6, [x1, #8]\n"
	"	stur	q28, [x1, #12]\n"
	"	stur	q29, [x1, #24]\n"
	"	stur	d5, [x1, #36]\n"
	"	str	w12, [x1, #44]\n"
	"	stur	q30, [x1, #48]\n"
	"	stur	q31, [x1, #60]\n"
	"	add	x1, x1, #72\n"
	"	b.ge	1b\n"
	"2:\n"
	"	adds	w2, w2, #2\n"
	"	b.eq	3f\n"
	"	ldr	s4, [x0]\n"
	"	ldur	s6, [x0, #8]\n"
	"	ldur	q8, [x0, #12]\n"
	"	ldur	h2, [x0, #4]\n"
	"	ldrh	w6, [x0, #6]\n"
	"	fmul	v20.4s, v16.4s, v4.s[0]\n"
	"	fmul	v21.4s, v17.4s, v4.s[0]\n"
	"	fmul	v22.4s, v18.4s, v4.s[0]\n"
	"	fmul	v23.4s, v19.4s, v4.s[0]\n"
	"	and	w7, w6, #0xf\n"
	"	and	w8, w6, #0xf0\n"
	"	orr	w7, w7, w8, lsl #4\n"
	"	and	w8, w6, #0xf00\n"
	"	orr	w7, w7, w8, lsl #8\n"
	"	and	w8, w6, #0xf000\n"
	"	orr	w7, w7, w8, lsl #12\n"
	"	orr	w6, w7, w7, lsl #4\n"
	"	orn	w15, w15, w6, asr #24\n"
	"	sshll	v6.8h, v6.8b, #0\n"
	"	sshll	v6.4s, v6.4h, #0\n"
	"	scvtf	v6.4s, v6.4s, #7\n"
	"	ushll	v2.8h, v2.8b, #0\n"
	"	ushll	v2.4s, v2.4h, #0\n"
	"	ucvtf	v2.2s, v2.2s, #7\n"
	"	fmul	v28.4s, v20.4s, v6.s[0]\n"
	"	fmul	v29.4s, v20.4s, v8.s[0]\n"
	"	mov	v4.8b, v1.8b\n"
	"	fmla	v28.4s, v21.4s, v6.s[1]\n"
	"	fmla	v29.4s, v21.4s, v8.s[1]\n"
	"	fmla	v4.2s, v2.2s, v0.2s\n"
	"	fmla	v28.4s, v22.4s, v6.s[2]\n"
	"	fmla	v29.4s, v22.4s, v8.s[2]\n"
	"	fadd	v29.4s, v29.4s, v23.4s\n"
	"	stur	d4, [x1]\n"
	"	str	w6, [x1, #8]\n"
	"	stur	q28, [x1, #12]\n"
	"	stur	q29, [x1, #24]\n"
	"3:\n"
	"	cmp	w15, #0\n"
	"	cset	w0, eq\n"
	"	ldp	d8, d9, [sp], #16\n"
	"	ret\n"
	".size VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf, .-VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf\n"
);
// W: float (2x, skinned)  T: u8 (prescaled)  C: 4444  N: s8  P: float (28 bytes in; 36 out, as the 1x): God of War's second most
// used skinned format. As VdecHand_Wf1_Tu8pre_C4444_Ns8_Pf, the blend matrix = bone0 * w0, + bone1 * w1 (the JIT's order).
// v16-v19 / v20-v23 the two bones' rows, v10-v13 / v24-v27 the two vertices' matrices (d8-d13 saved), v28-v31 the accumulators.
// int VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf(const u8 *src, u8 *dst, int count, const UVScale *uvScaleOffset, const float *boneMatrix)
__asm__(
	".text\n"
	".global VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf\n"
	".type VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf, %function\n"
	".p2align 4\n"
	"VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf:\n"
	"	stp	d8, d9, [sp, #-48]!\n"
	"	stp	d10, d11, [sp, #16]\n"
	"	stp	d12, d13, [sp, #32]\n"
	"	ldp	d0, d1, [x3]\n"
	"	mvni	v3.4s, #0\n"
	"	movi	v4.4s, #0\n"
	"	ext	v3.16b, v3.16b, v4.16b, #4\n"
	"	ld1	{ v4.4s, v5.4s, v6.4s }, [x4], #48\n"   /* bone 0 */
	"	ext	v7.16b, v6.16b, v6.16b, #4\n"
	"	ext	v6.16b, v5.16b, v6.16b, #8\n"
	"	ext	v5.16b, v4.16b, v5.16b, #12\n"
	"	and	v16.16b, v4.16b, v3.16b\n"
	"	and	v17.16b, v5.16b, v3.16b\n"
	"	and	v18.16b, v6.16b, v3.16b\n"
	"	and	v19.16b, v7.16b, v3.16b\n"
	"	ld1	{ v4.4s, v5.4s, v6.4s }, [x4]\n"         /* bone 1 */
	"	ext	v7.16b, v6.16b, v6.16b, #4\n"
	"	ext	v6.16b, v5.16b, v6.16b, #8\n"
	"	ext	v5.16b, v4.16b, v5.16b, #12\n"
	"	and	v20.16b, v4.16b, v3.16b\n"
	"	and	v21.16b, v5.16b, v3.16b\n"
	"	and	v22.16b, v6.16b, v3.16b\n"
	"	and	v23.16b, v7.16b, v3.16b\n"
	"	mov	w15, #0\n"
	"	subs	w2, w2, #2\n"
	"	b.lt	2f\n"
	"	.p2align 4\n"
	"1:\n"
	"	ldr	d4, [x0]\n"                    /* weights */
	"	ldr	d5, [x0, #28]\n"
	"	ldur	s6, [x0, #12]\n"               /* normals */
	"	ldur	s7, [x0, #40]\n"
	"	ldur	q8, [x0, #16]\n"               /* positions */
	"	ldur	q9, [x0, #44]\n"
	"	ldur	h2, [x0, #8]\n"                /* uv */
	"	ldur	h3, [x0, #36]\n"
	"	ldrh	w6, [x0, #10]\n"               /* colors */
	"	ldrh	w12, [x0, #38]\n"
	"	fmul	v10.4s, v16.4s, v4.s[0]\n"     /* the two matrices */
	"	fmul	v24.4s, v16.4s, v5.s[0]\n"
	"	and	w7, w6, #0xf\n"
	"	and	w8, w6, #0xf0\n"
	"	fmul	v11.4s, v17.4s, v4.s[0]\n"
	"	fmul	v25.4s, v17.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #4\n"
	"	and	w8, w6, #0xf00\n"
	"	fmul	v12.4s, v18.4s, v4.s[0]\n"
	"	fmul	v26.4s, v18.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #8\n"
	"	and	w8, w6, #0xf000\n"
	"	fmul	v13.4s, v19.4s, v4.s[0]\n"
	"	fmul	v27.4s, v19.4s, v5.s[0]\n"
	"	orr	w7, w7, w8, lsl #12\n"
	"	and	w13, w12, #0xf\n"
	"	fmla	v10.4s, v20.4s, v4.s[1]\n"
	"	fmla	v24.4s, v20.4s, v5.s[1]\n"
	"	orr	w6, w7, w7, lsl #4\n"          /* color A done */
	"	and	w14, w12, #0xf0\n"
	"	fmla	v11.4s, v21.4s, v4.s[1]\n"
	"	fmla	v25.4s, v21.4s, v5.s[1]\n"
	"	orr	w13, w13, w14, lsl #4\n"
	"	and	w14, w12, #0xf00\n"
	"	fmla	v12.4s, v22.4s, v4.s[1]\n"
	"	fmla	v26.4s, v22.4s, v5.s[1]\n"
	"	orr	w13, w13, w14, lsl #8\n"
	"	and	w14, w12, #0xf000\n"
	"	fmla	v13.4s, v23.4s, v4.s[1]\n"
	"	fmla	v27.4s, v23.4s, v5.s[1]\n"
	"	orr	w13, w13, w14, lsl #12\n"
	"	orn	w15, w15, w6, asr #24\n"
	"	sshll	v6.8h, v6.8b, #0\n"            /* normals to float */
	"	sshll	v7.8h, v7.8b, #0\n"
	"	orr	w12, w13, w13, lsl #4\n"       /* color B done */
	"	sshll	v6.4s, v6.4h, #0\n"
	"	sshll	v7.4s, v7.4h, #0\n"
	"	orn	w15, w15, w12, asr #24\n"
	"	scvtf	v6.4s, v6.4s, #7\n"
	"	scvtf	v7.4s, v7.4s, #7\n"
	"	ushll	v2.8h, v2.8b, #0\n"            /* uv to float */
	"	ushll	v3.8h, v3.8b, #0\n"
	"	ushll	v2.4s, v2.4h, #0\n"
	"	ushll	v3.4s, v3.4h, #0\n"
	"	ucvtf	v2.2s, v2.2s, #7\n"
	"	ucvtf	v3.2s, v3.2s, #7\n"
	"	fmul	v28.4s, v10.4s, v6.s[0]\n"     /* four chains */
	"	fmul	v29.4s, v10.4s, v8.s[0]\n"
	"	fmul	v30.4s, v24.4s, v7.s[0]\n"
	"	fmul	v31.4s, v24.4s, v9.s[0]\n"
	"	mov	v4.8b, v1.8b\n"
	"	mov	v5.8b, v1.8b\n"
	"	fmla	v28.4s, v11.4s, v6.s[1]\n"
	"	fmla	v29.4s, v11.4s, v8.s[1]\n"
	"	fmla	v30.4s, v25.4s, v7.s[1]\n"
	"	fmla	v31.4s, v25.4s, v9.s[1]\n"
	"	fmla	v4.2s, v2.2s, v0.2s\n"
	"	fmla	v5.2s, v3.2s, v0.2s\n"
	"	fmla	v28.4s, v12.4s, v6.s[2]\n"
	"	fmla	v29.4s, v12.4s, v8.s[2]\n"
	"	fmla	v30.4s, v26.4s, v7.s[2]\n"
	"	fmla	v31.4s, v26.4s, v9.s[2]\n"
	"	add	x0, x0, #56\n"
	"	subs	w2, w2, #2\n"
	"	fadd	v29.4s, v29.4s, v13.4s\n"
	"	fadd	v31.4s, v31.4s, v27.4s\n"
	"	stur	d4, [x1]\n"
	"	str	w6, [x1, #8]\n"
	"	stur	q28, [x1, #12]\n"
	"	stur	q29, [x1, #24]\n"
	"	stur	d5, [x1, #36]\n"
	"	str	w12, [x1, #44]\n"
	"	stur	q30, [x1, #48]\n"
	"	stur	q31, [x1, #60]\n"
	"	add	x1, x1, #72\n"
	"	b.ge	1b\n"
	"2:\n"
	"	adds	w2, w2, #2\n"
	"	b.eq	3f\n"
	"	ldr	d4, [x0]\n"
	"	ldur	s6, [x0, #12]\n"
	"	ldur	q8, [x0, #16]\n"
	"	ldur	h2, [x0, #8]\n"
	"	ldrh	w6, [x0, #10]\n"
	"	fmul	v10.4s, v16.4s, v4.s[0]\n"
	"	fmul	v11.4s, v17.4s, v4.s[0]\n"
	"	fmul	v12.4s, v18.4s, v4.s[0]\n"
	"	fmul	v13.4s, v19.4s, v4.s[0]\n"
	"	fmla	v10.4s, v20.4s, v4.s[1]\n"
	"	fmla	v11.4s, v21.4s, v4.s[1]\n"
	"	fmla	v12.4s, v22.4s, v4.s[1]\n"
	"	fmla	v13.4s, v23.4s, v4.s[1]\n"
	"	and	w7, w6, #0xf\n"
	"	and	w8, w6, #0xf0\n"
	"	orr	w7, w7, w8, lsl #4\n"
	"	and	w8, w6, #0xf00\n"
	"	orr	w7, w7, w8, lsl #8\n"
	"	and	w8, w6, #0xf000\n"
	"	orr	w7, w7, w8, lsl #12\n"
	"	orr	w6, w7, w7, lsl #4\n"
	"	orn	w15, w15, w6, asr #24\n"
	"	sshll	v6.8h, v6.8b, #0\n"
	"	sshll	v6.4s, v6.4h, #0\n"
	"	scvtf	v6.4s, v6.4s, #7\n"
	"	ushll	v2.8h, v2.8b, #0\n"
	"	ushll	v2.4s, v2.4h, #0\n"
	"	ucvtf	v2.2s, v2.2s, #7\n"
	"	fmul	v28.4s, v10.4s, v6.s[0]\n"
	"	fmul	v29.4s, v10.4s, v8.s[0]\n"
	"	mov	v4.8b, v1.8b\n"
	"	fmla	v28.4s, v11.4s, v6.s[1]\n"
	"	fmla	v29.4s, v11.4s, v8.s[1]\n"
	"	fmla	v4.2s, v2.2s, v0.2s\n"
	"	fmla	v28.4s, v12.4s, v6.s[2]\n"
	"	fmla	v29.4s, v12.4s, v8.s[2]\n"
	"	fadd	v29.4s, v29.4s, v13.4s\n"
	"	stur	d4, [x1]\n"
	"	str	w6, [x1, #8]\n"
	"	stur	q28, [x1, #12]\n"
	"	stur	q29, [x1, #24]\n"
	"3:\n"
	"	cmp	w15, #0\n"
	"	cset	w0, eq\n"
	"	ldp	d12, d13, [sp, #32]\n"
	"	ldp	d10, d11, [sp, #16]\n"
	"	ldp	d8, d9, [sp], #48\n"
	"	ret\n"
	".size VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf, .-VdecHand_Wf2_Tu8pre_C4444_Ns8_Pf\n"
);
#endif
