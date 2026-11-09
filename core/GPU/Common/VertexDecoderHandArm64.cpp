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
#endif
