/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_KFENCE_H
#define __ASM_KFENCE_H

#include <asm/set_memory.h>

static inline bool arch_kfence_init_pool(void) { return true; }

static inline bool kfence_protect_page(unsigned long addr, bool protect)
{
	set_memory_valid(addr, 1, !protect);

	return true;
}

#endif /* __ASM_KFENCE_H */
