/*
 * Written by Kanoj Sarcar, SGI, Aug 1999
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>

int numnodes = 1;	/* Initialized for UMA platforms */

static struct bootmem_data contig_bootmem_data;
struct pm_node contig_pm_node = { bdata: &contig_bootmem_data };

#ifndef CONFIG_DISCONTIGMEM

/*
 * This is meant to be invoked by platforms whose physical memory starts
 * at a considerably higher value than 0. Examples are Super-H, ARM, m68k.
 * Should be invoked with paramters (0, 0, unsigned long *[], start_paddr).
 */
void __init free_area_init_node(int nid, struct pm_node *pmnod, struct page *pmap,
	unsigned long *zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size)
{
	free_area_init_core(0, &contig_pm_node, &pg_map, zones_size,
				zone_start_paddr, zholes_size, pmap);
}

#endif /* !CONFIG_DISCONTIGMEM */

struct page * alloc_pages_node(int nid, unsigned int gfp_mask, unsigned int order)
{
#ifdef CONFIG_NUMA
	return __alloc_pages(gfp_mask, order, NODE_DATA(nid)->node_zonelists + (gfp_mask & GFP_ZONEMASK));
#else
	return alloc_pages(gfp_mask, order);
#endif
}

#ifdef CONFIG_DISCONTIGMEM

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

static spinlock_t node_lock = SPIN_LOCK_UNLOCKED;

void show_free_areas_node(struct pm_node *pmnod)
{
	unsigned long flags;

	spin_lock_irqsave(&node_lock, flags);
	show_free_areas_core(pmnod);
	spin_unlock_irqrestore(&node_lock, flags);
}

/*
 * Nodes can be initialized parallely, in no particular order.
 */
void __init free_area_init_node(int nid, struct pm_node *pmnod, struct page *pmap,
	unsigned long *zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size)
{
	int i, size = 0;
	struct page *discard;

	if (pg_map == (struct page *)NULL)
		pg_map = (struct page *)PAGE_OFFSET;

	free_area_init_core(nid, pmnod, &discard, zones_size, zone_start_paddr,
					zholes_size, pmap);
	pmnod->node_id = nid;

	/*
	 * Get space for the valid bitmap.
	 */
	for (i = 0; i < MAX_NR_ZONES; i++)
		size += zones_size[i];
	size = LONG_ALIGN((size + 7) >> 3);
	pmnod->valid_addr_bitmap = (unsigned long *)alloc_bootmem_node(pmnod, size);
	memset(pmnod->valid_addr_bitmap, 0, size);
}

static struct page * alloc_pages_pmnod(struct pm_node *pmnod, unsigned int gfp_mask,
	unsigned int order)
{
	return __alloc_pages(gfp_mask, order, pmnod->node_zonelists + (gfp_mask & GFP_ZONEMASK));
}

/*
 * This can be refined. Currently, tries to do round robin, instead
 * should do concentratic circle search, starting from current node.
 */
struct page * _alloc_pages(unsigned int gfp_mask, unsigned int order)
{
	struct page *ret = 0;
	struct pm_node *start, *temp;
#ifndef CONFIG_NUMA
	unsigned long flags;
	static pm_node *next = 0;
#endif

	if (order >= MAX_ORDER)
		return NULL;
#ifdef CONFIG_NUMA
	temp = NODE_DATA(numa_node_id());
#else
	spin_lock_irqsave(&node_lock, flags);
	if (!next) next = nod_list;
	temp = next;
	next = next->node_next;
	spin_unlock_irqrestore(&node_lock, flags);
#endif
	start = temp;
	while (temp) {
		if ((ret = alloc_pages_pmnod(temp, gfp_mask, order)))
			return(ret);
		temp = temp->node_next;
	}
	temp = nod_list;
	while (temp != start) {
		if ((ret = alloc_pages_pmnod(temp, gfp_mask, order)))
			return(ret);
		temp = temp->node_next;
	}
	return(0);
}

#endif /* CONFIG_DISCONTIGMEM */
