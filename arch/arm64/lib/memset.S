/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 ARM Ltd.
 * Copyright (C) 2013 Linaro.
 *
 * Pure-GPR AArch64 memset adapted from Arm Optimized Routines' overlapping
 * store structure with DC ZVA support for large zero fills.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Fill in the buffer with character c (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - buf
 *	x1 - c
 *	x2 - n
 * Returns:
 *	x0 - buf
 */

#define L(label) .L ## label

dstin		.req	x0
valin		.req	w1
count		.req	x2
dst		.req	x3
dstend		.req	x4
val		.req	x5
valw		.req	w5
zva_len		.req	x6
zva_lenw	.req	w6
tmp1		.req	x7
tmp1w		.req	w7
tmp2		.req	x8
tmp2w		.req	w8

SYM_FUNC_START_ALIAS(__memset)
SYM_FUNC_START_WEAK_PI(memset)
	and	valw, valin, #255
	orr	valw, valw, valw, lsl #8
	add	dstend, dstin, count
	orr	valw, valw, valw, lsl #16
	cmp	count, #96
	orr	val, val, val, lsl #32
	b.hi	L(set_long)

	cmp	count, #16
	b.hs	L(set16_96)

	/* 8..15 bytes */
	tbz	count, #3, L(set8)
	str	val, [dstin]
	str	val, [dstend, #-8]
	ret

	.p2align 3
	/* 4..7 bytes */
L(set8):
	tbz	count, #2, L(set4)
	str	valw, [dstin]
	str	valw, [dstend, #-4]
	ret

	/* 0..3 bytes branchless */
L(set4):
	cbz	count, L(set0)
	lsr	tmp1, count, #1
	strb	valw, [dstin]
	strb	valw, [dstin, tmp1]
	strb	valw, [dstend, #-1]
L(set0):
	ret

	.p2align 4
	/* 16..96 bytes */
L(set16_96):
	stp	val, val, [dstin]
	cmp	count, #32
	b.hi	L(set32_96)
	stp	val, val, [dstend, #-16]
	ret

	.p2align 4
L(set32_96):
	stp	val, val, [dstin, #16]
	stp	val, val, [dstend, #-32]
	stp	val, val, [dstend, #-16]
	cmp	count, #64
	b.ls	L(set0)
	stp	val, val, [dstin, #32]
	stp	val, val, [dstend, #-48]
	ret

	.p2align 4
	/* > 96 bytes */
L(set_long):
	bic	dst, dstin, #15
	stp	val, val, [dstin]
	cmp	count, #160
	ccmp	val, #0, #0, hs
	b.eq	L(try_zva)

L(no_zva):
	sub	count, dstend, dst
	sub	count, count, #(16 + 64)

	.p2align 4
L(loop64):
	stp	val, val, [dst, #16]
	stp	val, val, [dst, #32]
	stp	val, val, [dst, #48]
	stp	val, val, [dst, #64]!
	subs	count, count, #64
	b.hi	L(loop64)

L(tail64):
	stp	val, val, [dstend, #-64]
	stp	val, val, [dstend, #-48]
	stp	val, val, [dstend, #-32]
	stp	val, val, [dstend, #-16]
	ret

	.p2align 4
L(try_zva):
	mrs	tmp1, dczid_el0
	tbnz	tmp1, #4, L(no_zva)	/* Branch if DC ZVA is prohibited */
	and	tmp1w, tmp1w, #15
	mov	zva_lenw, #4
	lsl	zva_lenw, zva_lenw, tmp1w	/* zva_len = 4 << BS */
	tst	zva_lenw, #63
	b.ne	L(no_zva)		/* Skip ZVA if block size < 64 */

	sub	tmp1, zva_len, #1
	neg	tmp2, dstin
	and	tmp2, tmp2, tmp1	/* Bytes from dstin to next zva_len boundary */
	add	tmp1, tmp2, zva_len
	cmp	count, tmp1
	b.lo	L(no_zva)		/* Need at least one full ZVA block */

	add	tmp1, dstin, tmp2	/* tmp1 = aligned ZVA start */
	add	dst, dst, #16		/* [dstin .. dst-1] already zeroed */
	cmp	dst, tmp1
	b.hs	2f
1:
	stp	xzr, xzr, [dst], #16
	cmp	dst, tmp1
	b.lo	1b
2:
	mov	dst, tmp1
	sub	count, dstend, dst	/* Remaining bytes from ZVA start (>= zva_len) */
	sub	count, count, zva_len
3:
	dc	zva, dst
	add	dst, dst, zva_len
	subs	count, count, zva_len
	b.hs	3b

	adds	count, count, zva_len	/* Exact remaining tail bytes in [0, zva_len) */
	b.eq	L(set0)
	cmp	count, #64
	b.ls	L(tail64)
	sub	dst, dst, #16		/* zva_len >= 128 and tail > 64: finish via no_zva */
	b	L(no_zva)

SYM_FUNC_END_PI(memset)
EXPORT_SYMBOL(memset)
SYM_FUNC_END_ALIAS(__memset)
EXPORT_SYMBOL(__memset)
