/*
 *  linux/mm/bootmem.c
 *
 *  Copyright (C) 1999 Ingo Molnar
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *
 *  simple boot-time physical memory area allocator and
 *  free memory collector. It's used to deal with reserved
 *  system memory and memory holes as well.
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mmzone.h>
#include <asm/dma.h>
#include <asm/io.h>

/*
 * Access to this subsystem has to be serialized externally. (this is
 * true for the boot process anyway)
 */
unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;

/* return the number of _pages_ that will be allocated for the boot bitmap */
unsigned long __init bootmem_bootmap_pages (unsigned long pages)
{
	unsigned long mapsize;

	mapsize = (pages+7)/8;
	mapsize = (mapsize + ~PAGE_MASK) & PAGE_MASK;
	mapsize >>= PAGE_SHIFT;

	return mapsize;
}

/*
 * Called once to set up the allocator itself.
 */
static unsigned long __init init_bootmem_core (struct pm_node *pmnod,
	unsigned long mapstart, unsigned long start, unsigned long end)
{
	struct bootmem *botm = pmnod->botm;
	unsigned long mapsize = ((end - start)+7)/8;

	nod_list->node_next = nod_list;
	nod_list = pmnod;

	mapsize = (mapsize + (sizeof(long) - 1UL)) & ~(sizeof(long) - 1UL);
	botm->node_bootmem_map = phys_to_virt(mapstart << PAGE_SHIFT);
	botm->node_boot_start = (start << PAGE_SHIFT);
	botm->node_low_pfn = end;

	/*
	 * Initially all pages are reserved - setup_arch() has to
	 * register free RAM areas explicitly.
	 */
	memset(botm->node_bootmem_map, 0xff, mapsize);

	return mapsize;
}

/*
 * Marks a particular physical memory range as unallocatable. Usable RAM
 * might be used for boot-time allocations - or it might get added
 * to the free page pool later on.
 */
static void __init reserve_bootmem_core(struct bootmem *botm, unsigned long addr, unsigned long size)
{
	unsigned long i;
	/*
	 * round up, partially reserved pages are considered
	 * fully reserved.
	 */
	unsigned long sidx = (addr - botm->node_boot_start)/PAGE_SIZE;
	unsigned long eidx = (addr + size - botm->node_boot_start + 
							PAGE_SIZE-1)/PAGE_SIZE;
	unsigned long end = (addr + size + PAGE_SIZE-1)/PAGE_SIZE;

	if (!size) BUG();

	if (sidx < 0)
		BUG();
	if (eidx < 0)
		BUG();
	if (sidx >= eidx)
		BUG();
	if ((addr >> PAGE_SHIFT) >= botm->node_low_pfn)
		BUG();
	if (end > botm->node_low_pfn)
		BUG();
	for (i = sidx; i < eidx; i++)
		if (test_and_set_bit(i, botm->node_bootmem_map))
			printk("hm, page %08lx reserved twice.\n", i*PAGE_SIZE);
}

static void __init free_bootmem_core(struct bootmem *botm, unsigned long addr, unsigned long size)
{
	unsigned long i;
	unsigned long start;
	/*
	 * round down end of usable mem, partially free pages are
	 * considered reserved.
	 */
	unsigned long sidx;
	unsigned long eidx = (addr + size - botm->node_boot_start)/PAGE_SIZE;
	unsigned long end = (addr + size)/PAGE_SIZE;

	if (!size) BUG();
	if (end > botm->node_low_pfn)
		BUG();

	/*
	 * Round up the beginning of the address.
	 */
	start = (addr + PAGE_SIZE-1) / PAGE_SIZE;
	sidx = start - (botm->node_boot_start/PAGE_SIZE);

	for (i = sidx; i < eidx; i++) {
		if (!test_and_clear_bit(i, botm->node_bootmem_map))
			BUG();
	}
}

/*
 * We 'merge' subsequent allocations to save space. We might 'lose'
 * some fraction of a page if allocations cannot be satisfied due to
 * size constraints on boxes where there is physical RAM space
 * fragmentation - in these cases * (mostly large memory boxes) this
 * is not a problem.
 *
 * On low memory boxes we get it right in 100% of the cases.
 */

/*
 * alignment has to be a power of 2 value.
 */
static void * __init __alloc_bootmem_core (struct bootmem *botm, 
	unsigned long size, unsigned long align, unsigned long goal)
{
	unsigned long i, start = 0;
	void *ret;
	unsigned long offset, remaining_size;
	unsigned long areasize, preferred, incr;
	unsigned long eidx = botm->node_low_pfn - (botm->node_boot_start >>
							PAGE_SHIFT);

	if (!size) BUG();

	if (align & (align-1))
		BUG();

	offset = 0;
	if (align &&
	    (botm->node_boot_start & (align - 1UL)) != 0)
		offset = (align - (botm->node_boot_start & (align - 1UL)));
	offset >>= PAGE_SHIFT;

	/*
	 * We try to allocate bootmem pages above 'goal'
	 * first, then we try to allocate lower pages.
	 */
	if (goal && (goal >= botm->node_boot_start) && 
			((goal >> PAGE_SHIFT) < botm->node_low_pfn)) {
		preferred = goal - botm->node_boot_start;
	} else
		preferred = 0;

	preferred = ((preferred + align - 1) & ~(align - 1)) >> PAGE_SHIFT;
	preferred += offset;
	areasize = (size+PAGE_SIZE-1)/PAGE_SIZE;
	incr = align >> PAGE_SHIFT ? : 1;

restart_scan:
	for (i = preferred; i < eidx; i += incr) {
		unsigned long j;
		if (test_bit(i, botm->node_bootmem_map))
			continue;
		for (j = i + 1; j < i + areasize; ++j) {
			if (j >= eidx)
				goto fail_block;
			if (test_bit (j, botm->node_bootmem_map))
				goto fail_block;
		}
		start = i;
		goto found;
	fail_block:;
	}
	if (preferred) {
		preferred = offset;
		goto restart_scan;
	}
	return NULL;
found:
	if (start >= eidx)
		BUG();

	/*
	 * Is the next page of the previous allocation-end the start
	 * of this allocation's buffer? If yes then we can 'merge'
	 * the previous partial page with this allocation.
	 */
	if (align <= PAGE_SIZE
	    && botm->last_offset && botm->last_pos+1 == start) {
		offset = (botm->last_offset+align-1) & ~(align-1);
		if (offset > PAGE_SIZE)
			BUG();
		remaining_size = PAGE_SIZE-offset;
		if (size < remaining_size) {
			areasize = 0;
			// last_pos unchanged
			botm->last_offset = offset+size;
			ret = phys_to_virt(botm->last_pos*PAGE_SIZE + offset +
						botm->node_boot_start);
		} else {
			remaining_size = size - remaining_size;
			areasize = (remaining_size+PAGE_SIZE-1)/PAGE_SIZE;
			ret = phys_to_virt(botm->last_pos*PAGE_SIZE + offset +
						botm->node_boot_start);
			botm->last_pos = start+areasize-1;
			botm->last_offset = remaining_size;
		}
		botm->last_offset &= ~PAGE_MASK;
	} else {
		botm->last_pos = start + areasize - 1;
		botm->last_offset = size & ~PAGE_MASK;
		ret = phys_to_virt(start * PAGE_SIZE + botm->node_boot_start);
	}
	/*
	 * Reserve the area now:
	 */
	for (i = start; i < start+areasize; i++)
		if (test_and_set_bit(i, botm->node_bootmem_map))
			BUG();
	memset(ret, 0, size);
	return ret;
}

static unsigned long __init free_all_bootmem_core(struct pm_node *pmnod)
{
	struct page *page = pmnod->pg_map;
	struct bootmem *botm = pmnod->botm;
	unsigned long i, count, total = 0;
	unsigned long idx;

	if (!botm->node_bootmem_map) BUG();

	count = 0;
	idx = botm->node_low_pfn - (botm->node_boot_start >> PAGE_SHIFT);
	for (i = 0; i < idx; i++, page++) {
		if (!test_bit(i, botm->node_bootmem_map)) {
			count++;
			ClearPageReserved(page);
			set_page_count(page, 1);
			__free_page(page);
		}
	}
	total += count;

	/*
	 * Now free the allocator bitmap itself, it's not
	 * needed anymore:
	 */
	page = virt_to_page(botm->node_bootmem_map);
	count = 0;
	for (i = 0; i < ((botm->node_low_pfn-(botm->node_boot_start >> PAGE_SHIFT))/8 + PAGE_SIZE-1)/PAGE_SIZE; i++,page++) {
		count++;
		ClearPageReserved(page);
		set_page_count(page, 1);
		__free_page(page);
	}
	total += count;
	botm->node_bootmem_map = NULL;

	return total;
}

unsigned long __init init_bootmem_node (struct pm_node *pmnod, unsigned long freepfn, unsigned long startpfn, unsigned long endpfn)
{
	return(init_bootmem_core(pmnod, freepfn, startpfn, endpfn));
}

void __init reserve_bootmem_node (struct pm_node *pmnod, unsigned long physaddr, unsigned long size)
{
	reserve_bootmem_core(pmnod->botm, physaddr, size);
}

void __init free_bootmem_node (struct pm_node *pmnod, unsigned long physaddr, unsigned long size)
{
	return(free_bootmem_core(pmnod->botm, physaddr, size));
}

unsigned long __init free_all_bootmem_node (struct pm_node *pmnod)
{
	return(free_all_bootmem_core(pmnod));
}

unsigned long __init init_bootmem (unsigned long start, unsigned long pages)
{
	max_low_pfn = pages;
	min_low_pfn = start;
	return(init_bootmem_core(&contig_pm_node, start, 0, pages));
}

void __init reserve_bootmem (unsigned long addr, unsigned long size)
{
	reserve_bootmem_core(contig_pm_node.botm, addr, size);
}

void __init free_bootmem (unsigned long addr, unsigned long size)
{
	return(free_bootmem_core(contig_pm_node.botm, addr, size));
}

unsigned long __init free_all_bootmem (void)
{
	return(free_all_bootmem_core(&contig_pm_node));
}

void * __init __alloc_bootmem (unsigned long size, unsigned long align, unsigned long goal)
{
	struct pm_node *pmnod;
	void *ptr;

	for_each_pmnod(pmnod)
		if ((ptr = __alloc_bootmem_core(pmnod->botm, size,
						align, goal)))
			return(ptr);

	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

void * __init __alloc_bootmem_node (struct pm_node *pmnod, unsigned long size, unsigned long align, unsigned long goal)
{
	void *ptr;

	ptr = __alloc_bootmem_core(pmnod->botm, size, align, goal);
	if (ptr)
		return (ptr);

	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

