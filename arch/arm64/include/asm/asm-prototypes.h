/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/arm-smccc.h>

#include <asm/ftrace.h>
#include <asm/page.h>
#include <asm/string.h>
#include <linux/uaccess.h>

#include <asm-generic/asm-prototypes.h>

extern long long __ashlti3(long long a, int b);
extern long long __ashrti3(long long a, int b);
extern long long __lshrti3(long long a, int b);
