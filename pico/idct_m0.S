// 8x8 inverse DCT for the Cortex-M0+ (ARMv6-M Thumb), with the result stored to or added to the picture.
// Bit-exact with video_idct() in src/mpeg1/Frame.cpp followed by its clamped store.
//
//   void video_idct_put_m0(int32_t *block, uint8_t *dest, uint32_t stride, uint32_t masks);
//   void video_idct_add_m0(int32_t *block, uint8_t *dest, uint32_t stride, uint32_t masks);
//
// block holds 64 premultiplied coefficients in raster order and is left all zero. dest is word aligned.
// masks has two bits for each column j: bit 2j is set if the column has a coefficient below its first row,
// bit 2j + 1 if it has any coefficient. Columns with nothing below their first coefficient skip the
// arithmetic, and so do rows when only the first column has coefficients.
//
// GCC turns the multiplies into shifts and adds, but the RP2040's multiplier takes one cycle, and the M0+'s
// eight low registers are too few for GCC to keep a transform's values in them. In RAM, so the flash cache
// doesn't hold it up.

	.syntax unified
	.cpu cortex-m0plus
	.thumb

	// Stack frame, after the saved registers
	.equ MASK, 0
	.equ DEST, 4
	.equ STRIDE, 8
	.equ BLOCK, 12
	// Zero if only the first column has coefficients
	.equ ROWS_DC, 16
	.equ FRAME, 20

	// The odd half of a 1-D transform, from the coefficients at offsets p1, p3, p5, p7 from r0. Leaves
	// b7 in r10, x4 in r11, x0 in r12 and y7 in lr. r8 holds 473 and r9 362.
	.macro ODD p1, p3, p5, p7
	ldr r1, [r0, #\p1]
	ldr r3, [r0, #\p3]
	.if \p5 > 124
	adds r0, #128
	ldr r4, [r0, #(\p5 - 128)]
	ldr r2, [r0, #(\p7 - 128)]
	subs r0, #128
	.else
	ldr r4, [r0, #\p5]
	ldr r2, [r0, #\p7]
	.endif
	adds r5, r1, r2          // tmp1 = m1 + m7
	subs r1, r1, r2          // b6 = m1 - m7
	adds r2, r3, r4          // tmp2 = m3 + m5
	subs r4, r4, r3          // b4 = m5 - m3
	adds r3, r5, r2          // b7 = tmp1 + tmp2
	subs r5, r5, r2          // tmp1 - tmp2
	mov r2, r9
	muls r5, r2, r5
	adds r5, #128
	asrs r5, r5, #8          // ((tmp1 - tmp2) * 362 + 128) >> 8
	mov r2, r8
	muls r2, r1, r2          // b6 * 473
	movs r6, #196
	muls r6, r4, r6          // b4 * 196
	subs r2, r2, r6
	adds r2, #128
	asrs r2, r2, #8
	subs r2, r2, r3          // x4
	mov r6, r8
	muls r6, r4, r6          // b4 * 473
	movs r7, #196
	muls r7, r1, r7          // b6 * 196
	adds r6, r6, r7
	adds r6, #128
	asrs r6, r6, #8
	subs r5, r2, r5          // x0 = x4 - ...
	adds r6, r6, r5
	rsbs r6, r6, #0          // y7 = -x0 - ...
	mov r10, r3
	mov r11, r2
	mov r12, r5
	mov lr, r6
	.endm

	// The even half, from the coefficients at offsets p0, p2, p4, p6 from r0. Leaves y3 in r2, y5 in r3,
	// y6 in r4 and y4 in r7. With round set, adds 128 to m0, which every output includes once.
	.macro EVEN p0, p2, p4, p6, round
	ldr r1, [r0, #\p2]
	.if \p6 > 124
	adds r0, #128
	ldr r4, [r0, #(\p6 - 128)]
	ldr r2, [r0, #(\p4 - 128)]
	subs r0, #128
	.else
	ldr r4, [r0, #\p6]
	ldr r2, [r0, #\p4]
	.endif
	subs r7, r1, r4          // m2 - m6
	adds r1, r1, r4          // b3 = m2 + m6
	mov r4, r9
	muls r7, r4, r7
	adds r7, #128
	asrs r7, r7, #8
	subs r7, r7, r1          // x2
	ldr r4, [r0, #\p0]
	.if \round
	adds r4, #128
	.endif
	subs r3, r4, r2          // x1 = m0 - b1
	adds r4, r4, r2          // x3 = m0 + b1
	adds r2, r3, r7          // y3 = x1 + x2
	subs r3, r3, r7          // y5 = x1 - x2
	adds r7, r4, r1          // y4 = x3 + b3
	subs r4, r4, r1          // y6 = x3 - b3
	.endm

	// Clamps reg to 0..255, as 0 to 255 or -1 for above the range, which stores as 255
	.macro CLAMP reg
	cmp \reg, #255
	bls 1f
	mvns \reg, \reg
	asrs \reg, \reg, #31
1:
	.endm

	// Adds the picture's pixel at offset off from base to reg if add is set, clamps it and stores it there.
	// tmp is scratch.
	.macro STORE reg, base, off, tmp, add
	.if \add
	ldrb \tmp, [\base, #\off]
	adds \reg, \reg, \tmp
	.endif
	CLAMP \reg
	strb \reg, [\base, #\off]
	.endm

	// Shifts a rounded row output down and stores it at offset off from r0
	.macro EMIT reg, off, tmp, add
	asrs \reg, \reg, #8
	STORE \reg, r0, \off, \tmp, \add
	.endm

	.macro IDCT name, add
	.section .time_critical.\name, "ax", %progbits
	.global \name
	.type \name, %function
	.thumb_func
	.balign 4
\name:
	push {r4, r5, r6, r7, lr}
	mov r4, r8
	mov r5, r9
	mov r6, r10
	mov r7, r11
	push {r4, r5, r6, r7}
	sub sp, #FRAME
	str r3, [sp, #MASK]
	str r1, [sp, #DEST]
	str r2, [sp, #STRIDE]
	str r0, [sp, #BLOCK]
	lsrs r4, r3, #2
	str r4, [sp, #ROWS_DC]
	ldr r4, =473
	mov r8, r4
	ldr r4, =362
	mov r9, r4

	// Columns, while any are left with coefficients
.Lcolumn_\name:
	ldr r1, [sp, #MASK]
	cmp r1, #0
	beq .Lrows_\name
	lsrs r2, r1, #2
	str r2, [sp, #MASK]
	lsls r2, r1, #31
	beq 2f
	b .Lcolumn_full_\name
2:
	lsrs r1, r1, #2          // carry: the column's first coefficient is its only one
	bcc .Lcolumn_next_\name
	// Transforms to its first value in every row
	ldr r1, [r0, #0]
	str r1, [r0, #32]
	str r1, [r0, #64]
	str r1, [r0, #96]
	movs r2, #128
	adds r2, r2, r0
	str r1, [r2, #0]
	str r1, [r2, #32]
	str r1, [r2, #64]
	str r1, [r2, #96]
	b .Lcolumn_next_\name

.Lcolumn_full_\name:
	ODD 32, 96, 160, 224
	EVEN 0, 64, 128, 192, 0
	mov r1, r10              // b7
	mov r5, r11              // x4
	mov r6, r12              // x0
	adds r7, r7, r1          // out0 = y4 + b7
	lsls r1, r1, #1
	subs r1, r7, r1          // out7 = y4 - b7
	adds r2, r2, r5          // out1 = y3 + x4
	lsls r5, r5, #1
	subs r5, r2, r5          // out6 = y3 - x4
	subs r3, r3, r6          // out2 = y5 - x0
	lsls r6, r6, #1
	adds r6, r3, r6          // out5 = y5 + x0
	str r7, [r0, #0]
	str r2, [r0, #32]
	str r3, [r0, #64]
	mov r7, lr               // y7
	adds r3, r4, r7          // out4 = y6 + y7
	subs r4, r4, r7          // out3 = y6 - y7
	str r4, [r0, #96]
	adds r0, #128
	str r3, [r0, #0]
	str r6, [r0, #32]
	str r5, [r0, #64]
	str r1, [r0, #96]
	subs r0, #128

.Lcolumn_next_\name:
	adds r0, #4
	b .Lcolumn_\name

.Lrows_\name:
	ldr r0, [sp, #BLOCK]
	movs r1, #255
	adds r1, #1
	adds r1, r1, r0
	str r1, [sp, #BLOCK]     // now the end of the block
	ldr r1, [sp, #DEST]
	ldr r2, [sp, #ROWS_DC]
	cmp r2, #0
	beq 2f
	b .Lrow_full_\name
2:

	// Only the first column had coefficients: each row is its first value
	ldr r3, [sp, #STRIDE]
.Lrow_dc_\name:
	ldr r2, [r0, #0]
	adds r2, #128
	asrs r2, r2, #8
	movs r4, #0
	str r4, [r0, #0]
	adds r0, #32
	.if \add
	.irp off, 0, 1, 2, 3, 4, 5, 6, 7
	ldrb r4, [r1, #\off]
	adds r4, r4, r2
	CLAMP r4
	strb r4, [r1, #\off]
	.endr
	.else
	CLAMP r2
	uxtb r2, r2
	ldr r4, =0x01010101
	muls r2, r4, r2
	str r2, [r1, #0]
	str r2, [r1, #4]
	.endif
	adds r1, r1, r3
	ldr r4, [sp, #BLOCK]
	cmp r0, r4
	beq 2f
	b .Lrow_dc_\name
2:
	b .Ldone_\name

.Lrow_full_\name:
	// r0 the row, r1 the destination
	str r1, [sp, #DEST]
	ODD 4, 12, 20, 28
	EVEN 0, 8, 16, 24, 1
	mov r1, r10              // b7
	mov r10, r0              // the row, while r0 is the destination
	ldr r0, [sp, #DEST]
	adds r7, r7, r1          // out0
	lsls r1, r1, #1
	subs r1, r7, r1          // out7
	EMIT r7, 0, r5, \add
	EMIT r1, 7, r5, \add
	mov r5, r11              // x4
	adds r1, r2, r5          // out1
	subs r2, r2, r5          // out6
	EMIT r1, 1, r5, \add
	EMIT r2, 6, r5, \add
	mov r6, r12              // x0
	subs r1, r3, r6          // out2
	adds r3, r3, r6          // out5
	EMIT r1, 2, r5, \add
	EMIT r3, 5, r5, \add
	mov r6, lr               // y7
	subs r1, r4, r6          // out3
	adds r4, r4, r6          // out4
	EMIT r1, 3, r5, \add
	EMIT r4, 4, r5, \add
	ldr r1, [sp, #STRIDE]
	adds r1, r1, r0          // next destination row
	mov r0, r10
	movs r2, #0
	movs r3, #0
	movs r4, #0
	movs r5, #0
	stm r0!, {r2, r3, r4, r5}
	stm r0!, {r2, r3, r4, r5}
	ldr r2, [sp, #BLOCK]
	cmp r0, r2
	beq 2f
	b .Lrow_full_\name
2:
	b .Ldone_\name

.Ldone_\name:
	add sp, #FRAME
	pop {r4, r5, r6, r7}
	mov r8, r4
	mov r9, r5
	mov r10, r6
	mov r11, r7
	pop {r4, r5, r6, r7, pc}
	.ltorg
	.size \name, . - \name
	.endm

	IDCT video_idct_put_m0, 0
	IDCT video_idct_add_m0, 1
