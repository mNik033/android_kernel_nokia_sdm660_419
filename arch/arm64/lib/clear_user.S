/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Arm Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>

#ifdef CONFIG_ARM64_UAO
	.macro	uao_str_off, l, inst, alt_inst, reg, base, off
		alternative_if_not ARM64_HAS_UAO
8888:			\inst	\reg, [\base, #\off]
		alternative_else
			\alt_inst	\reg, [\base, #\off]
		alternative_endif
		_asm_extable	8888b, \l
	.endm
#else
	.macro	uao_str_off, l, inst, alt_inst, reg, base, off
		USER(\l, \inst \reg, [\base, #\off])
	.endm
#endif

	.text

/* Prototype: int __arch_clear_user(void *addr, size_t sz)
 * Purpose  : clear some user memory
 * Params   : addr - user memory address to clear
 *          : sz   - number of bytes to clear
 * Returns  : number of bytes NOT cleared
 *
 * Alignment fixed up by hardware.
 */
SYM_FUNC_START(__arch_clear_user)
	uaccess_enable_not_uao x2, x3, x4
	add	x2, x0, x1
	subs	x1, x1, #8
	b.mi	2f
	.p2align 4
1:
uao_user_alternative 9f, str, sttr, xzr, x0, 8
	subs	x1, x1, #8
	b.hi	1b
uao_str_off 9f, stur, sttr, xzr, x2, -8
	mov	x0, #0
	uaccess_disable_not_uao x2, x3
	ret

2:	tbz	x1, #2, 3f
uao_str_off 9f, str, sttr, wzr, x0, 0
uao_str_off 8f, stur, sttr, wzr, x2, -4
	b	5f

3:	tbz	x1, #1, 4f
uao_str_off 9f, strh, sttrh, wzr, x0, 0
4:	tbz	x1, #0, 5f
uao_str_off 7f, sturb, sttrb, wzr, x2, -1
5:	mov	x0, #0
	uaccess_disable_not_uao x2, x3
	ret
SYM_FUNC_END(__arch_clear_user)
EXPORT_SYMBOL(__arch_clear_user)

	.section .fixup,"ax"
	.align	2
7:	sub	x0, x2, #5	// Adjust for faulting on the final byte...
8:	add	x0, x0, #4	// ...or the second word of the 4-7 byte case
9:	sub	x0, x2, x0
	uaccess_disable_not_uao x2, x3
	ret
	.previous
