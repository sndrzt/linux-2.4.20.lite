/*
 * Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 */
#ifndef _LINUX_BOOTMEM_H
#define _LINUX_BOOTMEM_H

#include <asm/pgtable.h>
#include <asm/dma.h>
#include <linux/cache.h>
#include <linux/init.h>
#include <linux/mmzone.h>

/*
 *  simple boot-time physical memory area allocator.
 */

extern unsigned long max_low_pfn;
extern unsigned long min_low_pfn;
extern unsigned long max_pfn;

/*
 * node_bootmem_map is a map pointer - the bits represent all physical 
 * memory pages (including holes) on the node.
 */
struct bootmem {
	unsigned long node_boot_start;
	unsigned long node_low_pfn;
	void *node_bootmem_map;
	unsigned long last_offset;
	unsigned long last_pos;
};

extern unsigned long __init bootmem_bootmap_pages (unsigned long);
extern unsigned long __init init_bootmem (unsigned long addr, unsigned long memend);
extern void __init reserve_bootmem (unsigned long addr, unsigned long size);
extern void __init free_bootmem (unsigned long addr, unsigned long size);
extern void * __init __alloc_bootmem (unsigned long size, unsigned long align, unsigned long goal);
extern unsigned long __init free_all_bootmem (void);

#define alloc_bootmem_node(pmnod, x) \
	__alloc_bootmem((x), SMP_CACHE_BYTES, __pa(MAX_DMA_ADDRESS))

#endif /* _LINUX_BOOTMEM_H */
