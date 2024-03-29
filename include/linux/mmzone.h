#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>

/*
 * Free memory management - zoned buddy allocator.
 */

#define MAX_ORDER 10

struct free_area {
	struct list_head	free_list;
	unsigned long		*map;
};

struct pm_node;

/*
 * On machines where it is needed (eg PCs) we divide physical memory
 * into multiple physical zones. On a PC we have 3 zones:
 *
 * ZONE_DMA	  < 16 MB	ISA DMA capable memory
 * ZONE_NORMAL	16-896 MB	direct mapped by the kernel
 * ZONE_HIGHMEM	 > 896 MB	only page cache and user processes
 */
struct pm_zone {
	/*
	 * Commonly accessed fields:
	 */
	spinlock_t		lock;
	unsigned long		free_pages;
	unsigned long		pages_min, pages_low, pages_high;
	int			need_balance;

	/*
	 * free areas of different sizes
	 */
	struct free_area		free_areas[MAX_ORDER];

	/*
	 * wait_tbl		-- the array holding the hash table
	 * wait_table_size	-- the size of the hash table array
	 * wait_table_shift	-- wait_table_size
	 * 				== BITS_PER_LONG (1 << wait_table_bits)
	 *
	 * The purpose of all these is to keep track of the people
	 * waiting for a page to become available and make them
	 * runnable again when possible. The trouble is that this
	 * consumes a lot of space, especially when so few things
	 * wait on pages at a given time. So instead of using
	 * per-page waitqueues, we use a waitqueue hash table.
	 *
	 * The bucket discipline is to sleep on the same queue when
	 * colliding and wake all in that wait queue when removing.
	 * When something wakes, it must check to be sure its page is
	 * truly available, a la thundering herd. The cost of a
	 * collision is great, but given the expected load of the
	 * table, they should be so rare as to be outweighed by the
	 * benefits from the saved space.
	 *
	 * __wait_on_page() and unlock_page() in mm/filemap.c, are the
	 * primary users of these fields, and in mm/page_alloc.c
	 * free_area_init_core() performs the initialization of them.
	 */
	struct wait_queue_head_t	* wait_tbl;
	unsigned long		wait_table_size;
	unsigned long		wait_table_shift;

	/*
	 * Discontig memory support fields.
	 */
	struct pm_node	*zone_pmnod;
	struct page		*zone_pg_map;
	unsigned long		zone_start_paddr;
	unsigned long		zone_start_mapnr;

	/*
	 * rarely used fields:
	 */
	char			*name;
	unsigned long		size;
};

#define ZONE_DMA		0
#define ZONE_NORMAL		1
#define ZONE_HIGHMEM		2
#define MAX_NR_ZONES		3

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * Right now a zonelist takes up less than a cacheline. We never
 * modify it apart from boot-up, and only a few indices are used,
 * so despite the zonelist table being relatively big, the cache
 * footprint of this construct is very small.
 */
struct pm_zonelist {
	struct pm_zone * zones [MAX_NR_ZONES+1]; // NULL delimited
};

#define GFP_ZONEMASK	0x0f

/*
 * The pm_node structure is used in machines with CONFIG_DISCONTIGMEM
 * (mostly NUMA machines?) to denote a higher-level memory zone than the
 * pm_zone denotes.
 *
 * On NUMA machines, each NUMA node would have a struct pm_node to describe
 * it's memory layout.
 *
 * XXX: we need to move the global memory statistics (active_list, ...)
 *      into the struct pm_node to properly support NUMA.
 */
struct bootmem;
struct pm_node {
	struct pm_zone node_zones[MAX_NR_ZONES];
	struct pm_zonelist node_zonelists[GFP_ZONEMASK+1];
	int nr_zones;
	struct page *pg_map;
	unsigned long *valid_addr_bitmap;
	struct bootmem *botm;
	unsigned long node_start_paddr;
	unsigned long node_start_mapnr;
	unsigned long node_size;
	int node_id;
	struct pm_node *node_next;
};

extern int numnodes;
extern struct pm_node *nod_list;

#define memclass(pgzone, classzone)	(((pgzone)->zone_pmnod == (classzone)->zone_pmnod) \
			&& ((pgzone) <= (classzone)))

/*
 * The following two are not meant for general usage. They are here as
 * prototypes for the discontig memory code.
 */
struct page;
extern void show_free_areas_core(struct pm_node *pmnod);
extern void free_area_init_core(int nid, struct pm_node *pmnod, struct page **gmap,
  unsigned long *zones_size, unsigned long paddr, unsigned long *zholes_size,
  struct page *pmap);

extern struct pm_node contig_pm_node;

/*
 * next_zone - helper magic for for_each_zone()
 * Thanks to William Lee Irwin III for this piece of ingenuity.
 */
static inline struct pm_zone *next_zone(struct pm_zone *zone)
{
	struct pm_node *pmnod = zone->zone_pmnod;

	if (zone - pmnod->node_zones < MAX_NR_ZONES - 1)
		zone++;

	else if (pmnod->node_next) {
		pmnod = pmnod->node_next;
		zone = pmnod->node_zones;
	} else
		zone = NULL;

	return zone;
}

#define MAP_ALIGN(x)	((((x) % sizeof(struct page)) == 0) ? (x) : ((x) + \
		sizeof(struct page) - ((x) % sizeof(struct page))))

#endif /* !__ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _LINUX_MMZONE_H */
