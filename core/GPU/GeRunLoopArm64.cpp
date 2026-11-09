// Deadstation Portable: the GE command loop, GPUCommonHW::FastRunLoop's, in hand-written ARM64.
//
// void GeRunLoop(const GeLoopArgs *args)
// GeLoopArgs (GPUCommonHW.cpp): 0 gpu, 8 &list.pc, 16 base, 24 cmdmem, 32 handlers (a plain function address a command),
// 40 table (256 flag bytes: bit 0 FLAG_EXECUTE, bit 1 FLAG_EXECUTE | FLAG_EXECUTEONCHANGE, bit 2 FLAG_FLUSHBEFOREONCHANGE; then 256
// u64 dirty bits), 48 &gstate_c.dirty, 56 &the draw engine's queued vertices, 64 &downcount, 72 the flush thunk.
// Registers: x19 gpu, x20 &list.pc, x21 base, x22 cmdmem, x23 handlers, x24 dc, x25 pc, x26 &downcount, x27 table, x28 the dirty
// bits gathered (to the state before any call, and at the end); w1 this word, w17 the next, loaded while this one is decided
// (it reads one word past the last command run: list memory is mapped there).
// Per command: unchanged and not FLAG_EXECUTE, nothing; changed, the state takes the word, then its handler or its dirty bits;
// FLAG_FLUSHBEFOREONCHANGE with vertices queued, the draw engine's flush first. A handler: list.pc and downcount stored, called,
// both read back (it may move the list or end it). One prefetch a 64-byte line, four lines ahead.
// Raw cycles a command on the A53 at 1.8 GHz (local/brick/loopbench), against the compiled C++ loop it replaced: unchanged
// 12.0 (19.5), changed 17.0 (26.5), a handler call 27.6 (32.5), a 60/25/15 mix 23.5 (32.5).
#if defined(__aarch64__)
__asm__(
	".text\n"
	".global GeRunLoop\n"
	".type GeRunLoop, %function\n"
	".p2align 4\n"
	"GeRunLoop:\n"
	"	stp	x29, x30, [sp, #-144]!\n"
	"	mov	x29, sp\n"
	"	stp	x19, x20, [sp, #16]\n"
	"	stp	x21, x22, [sp, #32]\n"
	"	stp	x23, x24, [sp, #48]\n"
	"	stp	x25, x26, [sp, #64]\n"
	"	stp	x27, x28, [sp, #80]\n"
	"	str	x0, [sp, #96]\n"
	"	ldp	x19, x20, [x0]\n"
	"	ldp	x21, x22, [x0, #16]\n"
	"	ldp	x23, x27, [x0, #32]\n"
	"	ldr	x26, [x0, #64]\n"
	"	ldr	w24, [x26]\n"              // dc = downcount
	"	ldr	w25, [x20]\n"              // pc
	"	mov	x28, xzr\n"
	"	cmp	w24, #0\n"
	"	b.le	.Lge_done\n"
	".Lge_restart:\n"
	"	ldr	w1, [x21, x25]\n"          // this word
	".Lge_loop:\n"
	"	add	x11, x21, x25\n"
	"	lsr	w12, w1, #24\n"            // cmd
	"	ldr	w17, [x11, #4]\n"          // the next word, early
	"	ldrb	w15, [x27, x12]\n"         // flags
	"	ldr	w14, [x22, x12, lsl #2]\n"  // the state's word
	"	tst	w25, #0x3c\n"
	"	b.ne	1f\n"
	"	prfm	pldl1keep, [x11, #256]\n"  // a line's first word: the line four ahead
	"1:\n"
	"	eor	w2, w1, w14\n"             // diff
	"	cbnz	w2, .Lge_changed\n"
	"	tbnz	w15, #0, .Lge_call\n"      // unchanged: only FLAG_EXECUTE runs
	"	add	w25, w25, #4\n"
	"	mov	w1, w17\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lge_loop\n"
	"	b	.Lge_done\n"
	".Lge_changed:\n"
	"	tbnz	w15, #2, .Lge_maybeflush\n" // FLAG_FLUSHBEFOREONCHANGE: out of line
	".Lge_store:\n"
	"	str	w1, [x22, x12, lsl #2]\n"  // cmdmem[cmd] = op
	"	tbnz	w15, #1, .Lge_call\n"      // FLAG_EXECUTE | FLAG_EXECUTEONCHANGE
	"	add	x16, x27, x12, lsl #3\n"
	"	ldr	x16, [x16, #256]\n"        // its dirty bits
	"	add	w25, w25, #4\n"
	"	mov	w1, w17\n"
	"	orr	x28, x28, x16\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lge_loop\n"
	"	b	.Lge_done\n"
	".Lge_call:\n"                     // w1 op, w2 diff, w12 cmd
	"	ldr	x8, [x23, x12, lsl #3]\n"  // the handler
	"	cbnz	x28, .Lge_dirtycall\n"
	".Lge_call2:\n"
	"	str	w25, [x20]\n"              // list.pc
	"	str	w24, [x26]\n"              // downcount = dc
	"	mov	x0, x19\n"
	"	blr	x8\n"                      // handler(gpu, op, diff)
	"	ldr	w24, [x26]\n"              // dc = downcount
	"	ldr	w25, [x20]\n"              // the handler may have moved the list
	"	add	w25, w25, #4\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lge_restart\n"
	"	b	.Lge_done\n"
	".Lge_dirtycall:\n"                // the dirty bits gathered go to the state before any handler
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"	mov	x28, xzr\n"
	"	b	.Lge_call2\n"
	".Lge_maybeflush:\n"
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x9, [x0, #56]\n"
	"	ldr	w9, [x9]\n"
	"	cbz	w9, .Lge_store\n"          // nothing queued
	"	stp	x1, x2, [sp, #112]\n"
	"	stp	x12, x15, [sp, #128]\n"
	"	cbz	x28, 5f\n"
	"	ldr	x9, [x0, #48]\n"
	"	ldr	x10, [x9]\n"
	"	orr	x10, x10, x28\n"
	"	str	x10, [x9]\n"
	"	mov	x28, xzr\n"
	"5:\n"
	"	ldr	x9, [x0, #72]\n"
	"	mov	x0, x19\n"
	"	blr	x9\n"                      // the draw engine's flush (it doesn't move the list)
	"	ldp	x1, x2, [sp, #112]\n"
	"	ldp	x12, x15, [sp, #128]\n"
	"	add	x11, x21, x25\n"
	"	ldr	w17, [x11, #4]\n"          // the next word again
	"	b	.Lge_store\n"
	".Lge_done:\n"
	"	cbz	x28, 6f\n"
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"6:\n"
	"	str	w25, [x20]\n"
	"	str	wzr, [x26]\n"              // downcount = 0
	"	ldp	x19, x20, [sp, #16]\n"
	"	ldp	x21, x22, [sp, #32]\n"
	"	ldp	x23, x24, [sp, #48]\n"
	"	ldp	x25, x26, [sp, #64]\n"
	"	ldp	x27, x28, [sp, #80]\n"
	"	ldp	x29, x30, [sp], #144\n"
	"	ret\n"
	".size GeRunLoop, .-GeRunLoop\n"
);
#endif
