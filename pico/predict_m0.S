// Motion compensated prediction for the Cortex-M0+ (ARMv6-M Thumb), without averaging into the destination.
// Bit-exact with videoPredictBlock() in src/mpeg1/Frame.cpp.
//
//   void video_predict_m0_<words>_<half pel>_<offset>(uint8_t *dest, const uint8_t *source, uint32_t stride,
//                                                    uint32_t rows);
//
// Predicts a block of words * 4 pixels by rows from the source pixels at source + offset, where source is
// word aligned, into dest, which is too. half pel has the horizontal flag in bit 1 and the vertical one in
// bit 0. A function for each source alignment lets the realigning shifts be immediates. Each word column is
// done top to bottom, so with vertical half pel each source row is only read and realigned once.
// In RAM, so the flash cache doesn't hold it up.

	.syntax unified
	.cpu cortex-m0plus
	.thumb

	// dst = the word at source alignment sh bits, from aligned words a and b. tmp is scratch, and b
	// must not be dst unless sh is 32.
	.macro UNALIGN dst, a, b, sh, tmp
	.if \sh == 0
	.ifnc \dst, \a
	movs \dst, \a
	.endif
	.elseif \sh == 32
	movs \dst, \b
	.else
	lsrs \dst, \a, #\sh
	lsls \tmp, \b, #(32 - \sh)
	orrs \dst, \dst, \tmp
	.endif
	.endm

	// r6 = the realigned word at r0 for alignment sh. Uses r4, r5 and r7.
	.macro LOAD_WORD sh
	ldr r4, [r0, #0]
	.if \sh == 0
	movs r6, r4
	.else
	ldr r5, [r0, #4]
	UNALIGN r6, r4, r5, \sh, r7
	.endif
	.endm

	// r6 = the realigned word at r0 for alignment sh, r4 the one a pixel to its right
	.macro LOAD_PAIR sh
	ldr r4, [r0, #0]
	ldr r5, [r0, #4]
	UNALIGN r6, r4, r5, \sh, r7
	UNALIGN r4, r4, r5, (\sh + 8), r5
	.endm

	// For the four-pixel average: the 16-bit lane sums of r6 and r4's even pixels into r5, odd ones into r4.
	// r2 holds 0x00FF00FF.
	.macro LANE_SUMS
	movs r5, r6
	ands r5, r2
	movs r7, r4
	ands r7, r2
	adds r5, r5, r7
	lsrs r6, r6, #8
	ands r6, r2
	lsrs r4, r4, #8
	ands r4, r2
	adds r4, r4, r6
	.endm

	// out = (a + b + 1) >> 1 for each pixel. r2 holds 0x7F7F7F7F. a is clobbered.
	.macro AVERAGE2 out, a, b
	movs \out, \a
	orrs \out, \out, \b
	eors \a, \a, \b
	lsrs \a, \a, #1
	ands \a, \a, r2
	subs \out, \out, \a
	.endm

	// Without half pel, a row at a time: words + 1 aligned words in, realigned in place, words out
	.macro COPY words, offset
	.section .time_critical.video_predict_m0_\words\()_0_\offset, "ax", %progbits
	.global video_predict_m0_\words\()_0_\offset
	.type video_predict_m0_\words\()_0_\offset, %function
	.thumb_func
	.balign 4
video_predict_m0_\words\()_0_\offset:
	push {r4, r5, r6, r7, lr}
	movs r4, r0              // r0 the source, r1 the destination
	movs r0, r1
	movs r1, r4
	muls r3, r2, r3
	adds r3, r3, r1
	mov lr, r3               // the destination's end
	subs r2, #(4 * \words)
	mov r12, r2              // from the end of a row to the start of the next
1:
	.if \offset == 0
	.if \words == 4
	ldm r0!, {r2, r3, r4, r5}
	stm r1!, {r2, r3, r4, r5}
	.else
	ldm r0!, {r2, r3}
	stm r1!, {r2, r3}
	.endif
	add r0, r12
	.else
	.if \words == 4
	ldm r0!, {r2, r3, r4, r5, r6}
	UNALIGN r2, r2, r3, (\offset * 8), r7
	UNALIGN r3, r3, r4, (\offset * 8), r7
	UNALIGN r4, r4, r5, (\offset * 8), r7
	UNALIGN r5, r5, r6, (\offset * 8), r7
	stm r1!, {r2, r3, r4, r5}
	.else
	ldm r0!, {r2, r3, r4}
	UNALIGN r2, r2, r3, (\offset * 8), r7
	UNALIGN r3, r3, r4, (\offset * 8), r7
	stm r1!, {r2, r3}
	.endif
	add r0, r12
	subs r0, #4
	.endif
	add r1, r12
	cmp r1, lr
	bne 1b
	pop {r4, r5, r6, r7, pc}
	.size video_predict_m0_\words\()_0_\offset, . - video_predict_m0_\words\()_0_\offset
	.endm

	.macro PREDICT words, hp, offset
	.section .time_critical.video_predict_m0_\words\()_\hp\()_\offset, "ax", %progbits
	.global video_predict_m0_\words\()_\hp\()_\offset
	.type video_predict_m0_\words\()_\hp\()_\offset, %function
	.thumb_func
	.balign 4
video_predict_m0_\words\()_\hp\()_\offset:
	push {r4, r5, r6, r7, lr}
	mov r4, r8
	mov r5, r9
	mov r6, r10
	mov r7, r11
	push {r4, r5, r6, r7}
	movs r4, r0              // r0 the source, r1 the destination
	movs r0, r1
	movs r1, r4
	mov r10, r2              // stride
	muls r3, r2, r3
	mov r11, r3              // a column's height in bytes
	movs r4, #\words
	mov lr, r4               // columns left
	.if \hp == 3
	ldr r2, =0x00FF00FF
	ldr r3, =0x00020002
	.elseif \hp != 0
	ldr r2, =0x7F7F7F7F
	.endif

1:	// Each column: r0 the source, r1 the destination
	mov r4, r11
	adds r4, r4, r1
	mov r12, r4              // the column's end
	.if \hp == 3
	LOAD_PAIR (\offset * 8)
	LANE_SUMS
	mov r8, r5
	mov r9, r4
	add r0, r10
	.elseif \hp == 1
	LOAD_WORD (\offset * 8)
	movs r3, r6
	add r0, r10
	.endif

2:	// Each row
	.if \hp == 0
	LOAD_WORD (\offset * 8)
	str r6, [r1, #0]
	.elseif \hp == 2
	LOAD_PAIR (\offset * 8)
	AVERAGE2 r5, r6, r4
	str r5, [r1, #0]
	.elseif \hp == 1
	LOAD_WORD (\offset * 8)
	AVERAGE2 r5, r3, r6
	str r5, [r1, #0]
	movs r3, r6
	.else
	LOAD_PAIR (\offset * 8)
	LANE_SUMS
	mov r6, r8
	mov r7, r9
	mov r8, r5
	mov r9, r4
	adds r5, r5, r6
	adds r4, r4, r7
	adds r5, r5, r3
	adds r4, r4, r3
	lsrs r5, r5, #2
	ands r5, r2
	lsls r4, r4, #6
	bics r4, r2
	orrs r5, r5, r4
	str r5, [r1, #0]
	.endif
	add r0, r10
	add r1, r10
	cmp r1, r12
	bne 2b

	// Back to the top, one word right
	mov r4, r11
	subs r1, r1, r4
	adds r1, #4
	subs r0, r0, r4
	.if \hp & 1
	mov r4, r10
	subs r0, r0, r4
	.endif
	adds r0, #4
	mov r4, lr
	subs r4, #1
	mov lr, r4
	bne 1b

	pop {r4, r5, r6, r7}
	mov r8, r4
	mov r9, r5
	mov r10, r6
	mov r11, r7
	pop {r4, r5, r6, r7, pc}
	.ltorg
	.size video_predict_m0_\words\()_\hp\()_\offset, . - video_predict_m0_\words\()_\hp\()_\offset
	.endm

	.irp words, 2, 4
	.irp offset, 0, 1, 2, 3
	COPY \words, \offset
	.irp hp, 1, 2, 3
	PREDICT \words, \hp, \offset
	.endr
	.endr
	.endr
