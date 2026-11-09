// Deadstation Portable (experiment, DEADSTATION_PORTABLE_ASMLOOP=1): the GE command loop's scan, in hand-written ARM64.
//
// int GeFastScan(const GeFastScanArgs *args, u32 *pc, int dc)
// Runs display list commands from *pc, at most dc of them, as GPUCommonHW::FastRunLoop does, for the ones that need no call:
// unchanged commands that don't always execute (nothing to do), and changed ones that only store their word and dirty
// state (no handler, and no flush, or a flush with nothing queued). It stops at the first command that needs the C++ side
// (a handler or a flush), with *pc on it, and returns how many commands are left counting that one (0: all done).
// The dirty bits of the commands it ran are OR'd into the state cache's before it returns.
//
// GeFastScanArgs (GPUCommonHW.cpp): base, cmdmem, flags (a byte a command: bit 0 executes always, bit 1 executes when
// changed or always, bit 2 flushes first when changed), dirtyBits (a u64 a command), dirty (gstate_c.dirty), pending (the
// draw engine's queued vertex count).
//
// Leaf function: x0-x16 only (AAPCS64 scratch), no stack.
#if defined(__aarch64__)
__asm__(
	".text\n"
	".global GeFastScan\n"
	".type GeFastScan, %function\n"
	".p2align 4\n"
	"GeFastScan:\n"
	"	ldp	x3, x4, [x0]\n"            // base, cmdmem
	"	ldp	x5, x6, [x0, #16]\n"       // flags, dirtyBits
	"	ldp	x7, x8, [x0, #32]\n"       // dirty, pending
	"	ldr	w9, [x1]\n"                // pc (zero-extended)
	"	mov	x10, xzr\n"                // dirty bits gathered
	"	cmp	w2, #0\n"
	"	b.le	9f\n"
	"	.p2align 4\n"
	"1:\n"
	"	ldr	w12, [x3, x9]\n"           // op
	"	add	x11, x3, x9\n"
	"	prfm	pldl1keep, [x11, #256]\n"  // the list four lines ahead, as the C++ loop does
	"	lsr	w13, w12, #24\n"           // cmd
	"	ldrb	w15, [x5, x13]\n"          // its flags
	"	ldr	w14, [x4, x13, lsl #2]\n"  // the state's word for it
	"	add	x9, x9, #4\n"              // past it (undone if we stop on it)
	"	cmp	w12, w14\n"
	"	b.ne	3f\n"
	// unchanged: only an always-executing command needs a call
	"	tbnz	w15, #0, 7f\n"
	"	subs	w2, w2, #1\n"
	"	b.gt	1b\n"
	"	b	9f\n"
	// changed
	"3:\n"
	"	tbnz	w15, #1, 7f\n"             // a handler runs on change
	"	tbz	w15, #2, 4f\n"             // no flush before it
	"	ldr	w16, [x8]\n"
	"	cbnz	w16, 7f\n"                 // a flush with draws queued: the C++ side
	"4:\n"
	"	str	w12, [x4, x13, lsl #2]\n"  // the state takes the word
	"	ldr	x16, [x6, x13, lsl #3]\n"  // and its dirty bits
	"	subs	w2, w2, #1\n"
	"	orr	x10, x10, x16\n"
	"	b.gt	1b\n"
	"	b	9f\n"
	"7:\n"
	"	sub	x9, x9, #4\n"              // stop on this one
	"9:\n"
	"	str	w9, [x1]\n"
	"	cbz	x10, 10f\n"
	"	ldr	x16, [x7]\n"
	"	orr	x16, x16, x10\n"
	"	str	x16, [x7]\n"
	"10:\n"
	"	mov	w0, w2\n"
	"	ret\n"
	".size GeFastScan, .-GeFastScan\n"
);
#endif

// void GeFastRunLoop(const GeLoopArgs *args): all of GPUCommonHW::FastRunLoop (DEADSTATION_PORTABLE_ASMLOOP=2), handlers called from here.
// GeLoopArgs (GPUCommonHW.cpp): 0 gpu, 8 &list.pc, 16 base, 24 cmdmem, 32 cmdInfo (24-byte rows: flags, then the member
// pointer: function or vtable offset, this adjustment << 1 | virtual), 40 flags bytes, 48 &gstate_c.dirty, 56 &queued vertices,
// 64 &downcount, 72 dirty bits a command, 80 the flush thunk.
// In registers across the loop: x19 gpu, x20 &list.pc, x21 base, x22 cmdmem, x23 cmdInfo, x24 dc, x25 pc, x26 flags bytes,
// x27 dirty bits table, x28 dirty bits gathered (written to the state before any call, as the C++ loop sets them at once).
// list.pc and downcount go to memory around a handler and are read back after it, as the C++ loop does.
#if defined(__aarch64__)
__asm__(
	".text\n"
	".global GeFastRunLoop\n"
	".type GeFastRunLoop, %function\n"
	".p2align 4\n"
	"GeFastRunLoop:\n"
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
	"	ldp	x23, x26, [x0, #32]\n"
	"	ldr	x27, [x0, #72]\n"
	"	ldr	x9, [x0, #64]\n"
	"	ldr	w24, [x9]\n"               // dc = downcount
	"	ldr	w25, [x20]\n"              // pc
	"	mov	x28, xzr\n"
	"	cmp	w24, #0\n"
	"	b.le	.Lgfr_done\n"
	"	.p2align 4\n"
	".Lgfr_loop:\n"
	"	ldr	w1, [x21, x25]\n"          // op
	"	add	x11, x21, x25\n"
	"	prfm	pldl1keep, [x11, #256]\n"
	"	lsr	w12, w1, #24\n"            // cmd
	"	ldrb	w15, [x26, x12]\n"
	"	ldr	w14, [x22, x12, lsl #2]\n"
	"	eor	w2, w1, w14\n"             // diff
	"	cbnz	w2, .Lgfr_changed\n"
	"	tbnz	w15, #0, .Lgfr_call\n"     // unchanged: only FLAG_EXECUTE runs
	".Lgfr_next:\n"
	"	add	w25, w25, #4\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lgfr_loop\n"
	"	b	.Lgfr_done\n"
	".Lgfr_changed:\n"
	"	tbz	w15, #2, .Lgfr_store\n"     // FLAG_FLUSHBEFOREONCHANGE?
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x9, [x0, #56]\n"
	"	ldr	w9, [x9]\n"
	"	cbz	w9, .Lgfr_store\n"          // nothing queued
	"	stp	x1, x2, [sp, #112]\n"
	"	stp	x12, x15, [sp, #128]\n"
	"	cbz	x28, 1f\n"
	"	ldr	x9, [x0, #48]\n"
	"	ldr	x10, [x9]\n"
	"	orr	x10, x10, x28\n"
	"	str	x10, [x9]\n"
	"	mov	x28, xzr\n"
	"1:\n"
	"	ldr	x9, [x0, #80]\n"
	"	mov	x0, x19\n"
	"	blr	x9\n"                      // the draw engine's flush
	"	ldp	x1, x2, [sp, #112]\n"
	"	ldp	x12, x15, [sp, #128]\n"
	".Lgfr_store:\n"
	"	str	w1, [x22, x12, lsl #2]\n"  // cmdmem[cmd] = op
	"	tbnz	w15, #1, .Lgfr_call\n"     // FLAG_EXECUTE | FLAG_EXECUTEONCHANGE
	"	ldr	x16, [x27, x12, lsl #3]\n"
	"	orr	x28, x28, x16\n"
	"	b	.Lgfr_next\n"
	".Lgfr_call:\n"
	"	mov	w9, #24\n"
	"	umaddl	x10, w12, w9, x23\n"       // &cmdInfo[cmd]
	"	ldp	x8, x9, [x10, #8]\n"       // member pointer
	"	ldr	x0, [sp, #96]\n"
	"	cbz	x28, 2f\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"	mov	x28, xzr\n"
	"2:\n"
	"	str	w25, [x20]\n"              // list.pc
	"	ldr	x10, [x0, #64]\n"
	"	str	w24, [x10]\n"              // downcount = dc
	"	add	x0, x19, x9, asr #1\n"     // this, adjusted
	"	tbz	w9, #0, 3f\n"
	"	ldr	x10, [x0]\n"               // virtual: from the vtable
	"	ldr	x8, [x10, x8]\n"
	"3:\n"
	"	blr	x8\n"                      // (this->*func)(op, diff)
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x10, [x0, #64]\n"
	"	ldr	w24, [x10]\n"              // dc = downcount
	"	ldr	w25, [x20]\n"              // the handler may have moved the list
	"	b	.Lgfr_next\n"
	".Lgfr_done:\n"
	"	ldr	x0, [sp, #96]\n"
	"	cbz	x28, 4f\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"4:\n"
	"	str	w25, [x20]\n"
	"	ldr	x10, [x0, #64]\n"
	"	str	wzr, [x10]\n"              // downcount = 0
	"	ldp	x19, x20, [sp, #16]\n"
	"	ldp	x21, x22, [sp, #32]\n"
	"	ldp	x23, x24, [sp, #48]\n"
	"	ldp	x25, x26, [sp, #64]\n"
	"	ldp	x27, x28, [sp, #80]\n"
	"	ldp	x29, x30, [sp], #144\n"
	"	ret\n"
	".size GeFastRunLoop, .-GeFastRunLoop\n"
);
#endif

// void GeFastRunLoop3(const GeLoopArgs *args) (DEADSTATION_PORTABLE_ASMLOOP=3): GeFastRunLoop, scheduled for the A53.
// - The next word is loaded while this one is decided (its load-to-use latency under this command's work, in order or not).
//   It reads one word past the last command run: list memory is mapped there (a list's END is never the last word of RAM).
// - One prefetch a 64-byte line, 256 bytes ahead, not one a word.
// - After a handler (which may have decoded thousands of vertices and pushed the list out of L1), the next four lines are
//   asked for again before going on.
// Same registers and arguments as GeFastRunLoop; w17 the next word.
#if defined(__aarch64__)
__asm__(
	".text\n"
	".global GeFastRunLoop3\n"
	".type GeFastRunLoop3, %function\n"
	".p2align 4\n"
	"GeFastRunLoop3:\n"
	"	stp	x29, x30, [sp, #-144]!\n"
	"	mov	x29, sp\n"
	"	stp	x19, x20, [sp, #16]\n"
	"	stp	x21, x22, [sp, #32]\n"
	"	stp	x23, x24, [sp, #48]\n"
	"	stp	x25, x26, [sp, #64]\n"
	"	stp	x27, x28, [sp, #80]\n"
	"	str	x0, [sp, #96]\n"
	"	ldr	x9, [x0, #64]\n"
	"	str	x9, [sp, #104]\n"         // &downcount, in the frame
	"	ldp	x19, x20, [x0]\n"
	"	ldp	x21, x22, [x0, #16]\n"
	"	ldp	x23, x26, [x0, #32]\n"
	"	ldr	x27, [x0, #72]\n"
	"	ldr	x9, [x0, #64]\n"
	"	ldr	w24, [x9]\n"               // dc = downcount
	"	ldr	w25, [x20]\n"              // pc
	"	mov	x28, xzr\n"
	"	cmp	w24, #0\n"
	"	b.le	.Lg3_done\n"
	".Lg3_restart:\n"                  // (from the entry and after a handler) prime the lines ahead and the first word
	"	add	x11, x21, x25\n"
	"	prfm	pldl1keep, [x11, #64]\n"
	"	prfm	pldl1keep, [x11, #128]\n"
	"	prfm	pldl1keep, [x11, #192]\n"
	"	prfm	pldl1keep, [x11, #256]\n"
	"	ldr	w1, [x11]\n"
	"	.p2align 4\n"
	".Lg3_loop:\n"                     // w1 = this word, x25 = its pc
	"	lsr	w12, w1, #24\n"            // cmd
	"	add	x11, x21, x25\n"
	"	ldrb	w15, [x26, x12]\n"
	"	ldr	w14, [x22, x12, lsl #2]\n"
	"	ldr	w17, [x11, #4]\n"          // the next word, early
	"	tst	w25, #0x3c\n"              // first word of a line: the line four ahead
	"	b.ne	1f\n"
	"	prfm	pldl1keep, [x11, #256]\n"
	"1:\n"
	"	eor	w2, w1, w14\n"             // diff
	"	cbnz	w2, .Lg3_changed\n"
	"	tbnz	w15, #0, .Lg3_call\n"      // unchanged: only FLAG_EXECUTE runs
	".Lg3_next:\n"
	"	add	w25, w25, #4\n"
	"	mov	w1, w17\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lg3_loop\n"
	"	b	.Lg3_done\n"
	".Lg3_changed:\n"
	"	tbz	w15, #2, .Lg3_store\n"
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x9, [x0, #56]\n"
	"	ldr	w9, [x9]\n"
	"	cbz	w9, .Lg3_store\n"
	"	stp	x1, x2, [sp, #112]\n"
	"	stp	x12, x15, [sp, #128]\n"
	"	cbz	x28, 1f\n"
	"	ldr	x9, [x0, #48]\n"
	"	ldr	x10, [x9]\n"
	"	orr	x10, x10, x28\n"
	"	str	x10, [x9]\n"
	"	mov	x28, xzr\n"
	"1:\n"
	"	ldr	x9, [x0, #80]\n"
	"	mov	x0, x19\n"
	"	blr	x9\n"                      // the draw engine's flush (it doesn't move the list)
	"	ldp	x1, x2, [sp, #112]\n"
	"	ldp	x12, x15, [sp, #128]\n"
	"	add	x11, x21, x25\n"
	"	ldr	w17, [x11, #4]\n"          // the next word again (w17 didn't survive the call)
	".Lg3_store:\n"
	"	str	w1, [x22, x12, lsl #2]\n"  // cmdmem[cmd] = op
	"	tbnz	w15, #1, .Lg3_call\n"
	"	ldr	x16, [x27, x12, lsl #3]\n"
	"	orr	x28, x28, x16\n"
	"	b	.Lg3_next\n"
	".Lg3_call:\n"
	"	mov	w9, #24\n"
	"	umaddl	x10, w12, w9, x23\n"
	"	ldp	x8, x9, [x10, #8]\n"       // member pointer
	"	cbz	x28, 2f\n"
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"	mov	x28, xzr\n"
	"2:\n"
	"	str	w25, [x20]\n"
	"	ldr	x10, [sp, #104]\n"
	"	str	w24, [x10]\n"
	"	add	x0, x19, x9, asr #1\n"
	"	tbz	w9, #0, 3f\n"
	"	ldr	x10, [x0]\n"
	"	ldr	x8, [x10, x8]\n"
	"3:\n"
	"	blr	x8\n"                      // (this->*func)(op, diff)
	"	ldr	x10, [sp, #104]\n"
	"	ldr	w24, [x10]\n"              // dc = downcount
	"	ldr	w25, [x20]\n"              // the handler may have moved the list
	"	add	w25, w25, #4\n"
	"	subs	w24, w24, #1\n"
	"	b.gt	.Lg3_restart\n"
	".Lg3_done:\n"
	"	cbz	x28, 4f\n"
	"	ldr	x0, [sp, #96]\n"
	"	ldr	x10, [x0, #48]\n"
	"	ldr	x11, [x10]\n"
	"	orr	x11, x11, x28\n"
	"	str	x11, [x10]\n"
	"4:\n"
	"	str	w25, [x20]\n"
	"	ldr	x10, [sp, #104]\n"
	"	str	wzr, [x10]\n"
	"	ldp	x19, x20, [sp, #16]\n"
	"	ldp	x21, x22, [sp, #32]\n"
	"	ldp	x23, x24, [sp, #48]\n"
	"	ldp	x25, x26, [sp, #64]\n"
	"	ldp	x27, x28, [sp, #80]\n"
	"	ldp	x29, x30, [sp], #144\n"
	"	ret\n"
	".size GeFastRunLoop3, .-GeFastRunLoop3\n"
);
#endif
