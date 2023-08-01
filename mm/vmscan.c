/*
 *  linux/mm/vmscan.c
 *
 *  The pageout daemon, decides which pages to evict (swap out) and
 *  does the actual work of freeing them.
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/smp_lock.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/file.h>

#include <asm/pgalloc.h>

/*
 * The "priority" of VM scanning is how much of the queues we
 * will scan in one go. A value of 6 for DEF_PRIORITY implies
 * that we'll scan 1/64th of the queues ("queue_length >> 6")
 * during a normal aging round.
 */
#define DEF_PRIORITY (6)

/*
 * The swap-out function returns 1 if it successfully
 * scanned all the pages it was asked to (`count').
 * It returns zero if it couldn't do anything,
 *
 * rss may decrease because pages are shared, but this
 * doesn't count as having freed a page.
 */

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int try_to_swap_out(struct mm_struct * mm, struct vm_area_struct* vma, unsigned long address, pte_t * page_table, struct page *page, struct pm_zone * classzone)
{
	pte_t pte;
	swp_entry_t entry;

	/* Don't look at this pte if it's been accessed recently. */
	if ((vma->vm_flags & VM_LOCKED) || ptep_test_and_clear_young(page_table)) {
		mark_page_accessed(page);
		return 0;
	}

	/* Don't bother unmapping pages that are active */
	if (PageActive(page))
		return 0;

	/* Don't bother replenishing zones not under pressure.. */
	if (!memclass(page_zone(page), classzone))
		return 0;

	if (TryLockPage(page))
		return 0;

	/* From this point on, the odds are that we're going to
	 * nuke this pte, so read and clear the pte.  This hook
	 * is needed on CPUs which update the accessed and dirty
	 * bits in hardware.
	 */
	flush_cache_page(vma, address);
	pte = ptep_get_and_clear(page_table);
	flush_tlb_page(vma, address);

	if (pte_dirty(pte))
		set_page_dirty(page);

	/*
	 * Is the page already in the swap cache? If so, then
	 * we can just drop our reference to it without doing
	 * any IO - it's already up-to-date on disk.
	 */
	if (PageSwapCache(page)) {
		entry.val = page->index;
		swap_duplicate(entry);
set_swap_pte:
		set_pte(page_table, swp_entry_to_pte(entry));
drop_pte:
		mm->rss--;
		UnlockPage(page);
		{
			int freeable = page_count(page) - !!page->buffers <= 2;
			page_cache_release(page);
			return freeable;
		}
	}

	/*
	 * Is it a clean page? Then it must be recoverable
	 * by just paging it in again, and we can just drop
	 * it..  or if it's dirty but has backing store,
	 * just mark the page dirty and drop it.
	 *
	 * However, this won't actually free any real
	 * memory, as the page will just be in the page cache
	 * somewhere, and as such we should just continue
	 * our scan.
	 *
	 * Basically, this just makes it possible for us to do
	 * some real work in the future in "refill_inactive()".
	 */
	if (page->mapping)
		goto drop_pte;
	if (!PageDirty(page))
		goto drop_pte;

	/*
	 * Anonymous buffercache pages can be left behind by
	 * concurrent truncate and pagefault.
	 */
	if (page->buffers)
		goto preserve;

	/*
	 * This is a dirty, swappable page.  First of all,
	 * get a suitable swap entry for it, and make sure
	 * we have the swap cache set up to associate the
	 * page with that swap entry.
	 */
	for (;;) {
		entry = get_swap_page();
		if (!entry.val)
			break;
		/* Add it to the swap cache and mark it dirty
		 * (adding to the page cache will clear the dirty
		 * and uptodate bits, so we need to do it again)
		 */
		if (add_to_swap_cache(page, entry) == 0) {
			SetPageUptodate(page);
			set_page_dirty(page);
			goto set_swap_pte;
		}
		/* Raced with "speculative" read_swap_cache_async */
		swap_free(entry);
	}

	/* No swap space left */
preserve:
	set_pte(page_table, pte);
	UnlockPage(page);
	return 0;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_pmd(struct mm_struct * mm, struct vm_area_struct * vma, pmd_t *dir, unsigned long address, unsigned long end, int count, struct pm_zone * classzone)
{
	pte_t * pte;
	unsigned long pmd_end;

	if (pmd_none(*dir))
		return count;
	if (pmd_bad(*dir)) {
		pmd_ERROR(*dir);
		pmd_clear(dir);
		return count;
	}
	
	pte = pte_offset(dir, address);
	
	pmd_end = (address + PMD_SIZE) & PMD_MASK;
	if (end > pmd_end)
		end = pmd_end;

	do {
		if (pte_present(*pte)) {
			struct page *page = pte_page(*pte);

			if (VALID_PAGE(page) && !PageReserved(page)) {
				count -= try_to_swap_out(mm, vma, address, pte, page, classzone);
				if (!count) {
					address += PAGE_SIZE;
					break;
				}
			}
		}
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));
	mm->swap_address = address;
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_pgd(struct mm_struct * mm, struct vm_area_struct * vma, pgd_t *dir, unsigned long address, unsigned long end, int count, struct pm_zone * classzone)
{
	pmd_t * pmd;
	unsigned long pgd_end;

	if (pgd_none(*dir))
		return count;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return count;
	}

	pmd = pmd_offset(dir, address);

	pgd_end = (address + PGDIR_SIZE) & PGDIR_MASK;	
	if (pgd_end && (end > pgd_end))
		end = pgd_end;
	
	do {
		count = swap_out_pmd(mm, vma, pmd, address, end, count, classzone);
		if (!count)
			break;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_vma(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, int count, struct pm_zone * classzone)
{
	pgd_t *pgdir;
	unsigned long end;

	/* Don't swap out areas which are reserved */
	if (vma->vm_flags & VM_RESERVED)
		return count;

	pgdir = pgd_offset(mm, address);

	end = vma->vm_end;
	BUG_ON(address >= end);
	do {
		count = swap_out_pgd(mm, vma, pgdir, address, end, count, classzone);
		if (!count)
			break;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		pgdir++;
	} while (address && (address < end));
	return count;
}

/* Placeholder for swap_out(): may be updated by fork.c:mmput() */
struct mm_struct *swap_mm = &init_mm;

/*
 * Returns remaining count of pages to be swapped out by followup call.
 */
static inline int swap_out_mm(struct mm_struct * mm, int count, int * mmcounter, struct pm_zone * classzone)
{
	unsigned long address;
	struct vm_area_struct* vma;

	/*
	 * Find the proper vm-area after freezing the vma chain 
	 * and ptes.
	 */
	spin_lock(&mm->page_table_lock);
	address = mm->swap_address;
	if (address == TASK_SIZE || swap_mm != mm) {
		/* We raced: don't count this mm but try again */
		++*mmcounter;
		goto out_unlock;
	}
	vma = find_vma(mm, address);
	if (vma) {
		if (address < vma->vm_start)
			address = vma->vm_start;

		for (;;) {
			count = swap_out_vma(mm, vma, address, count, classzone);
			vma = vma->vm_next;
			if (!vma)
				break;
			if (!count)
				goto out_unlock;
			address = vma->vm_start;
		}
	}
	/* Indicate that we reached the end of address space */
	mm->swap_address = TASK_SIZE;

out_unlock:
	spin_unlock(&mm->page_table_lock);
	return count;
}

static int FASTCALL(swap_out(unsigned int priority, unsigned int gfp_mask, struct pm_zone * classzone));
static int swap_out(unsigned int priority, unsigned int gfp_mask, struct pm_zone * classzone)
{
	int counter, nr_pages = SWAP_CLUSTER_MAX;
	struct mm_struct *mm;

	counter = mmlist_nr;
	do {
		if (unlikely(current->need_resched)) {
			__set_current_state(TASK_RUNNING);
			schedule();
		}

		spin_lock(&mmlist_lock);
		mm = swap_mm;
		while (mm->swap_address == TASK_SIZE || mm == &init_mm) {
			mm->swap_address = 0;
			mm = list_entry(mm->mmlist.next, struct mm_struct, mmlist);
			if (mm == swap_mm)
				goto empty;
			swap_mm = mm;
		}

		/* Make sure the mm doesn't disappear when we drop the lock.. */
		atomic_inc(&mm->mm_users);
		spin_unlock(&mmlist_lock);

		nr_pages = swap_out_mm(mm, nr_pages, &counter, classzone);

		mmput(mm);

		if (!nr_pages)
			return 1;
	} while (--counter >= 0);

	return 0;

empty:
	spin_unlock(&mmlist_lock);
	return 0;
}

static int FASTCALL(shrink_cache(int nr_pages, struct pm_zone * classzone, unsigned int gfp_mask, int priority));
static int shrink_cache(int nr_pages, struct pm_zone * classzone, unsigned int gfp_mask, int priority)
{
	struct list_head * entry;
	int max_scan = nr_inactive_pages / priority;
	int max_mapped = min((nr_pages << (10 - priority)), max_scan / 10);

	spin_lock(&pagemap_lru_lock);
	while (--max_scan >= 0 && (entry = inactive_list.prev) != &inactive_list) {
		struct page * page;

		if (unlikely(current->need_resched)) {
			spin_unlock(&pagemap_lru_lock);
			__set_current_state(TASK_RUNNING);
			schedule();
			spin_lock(&pagemap_lru_lock);
			continue;
		}

		page = list_entry(entry, struct page, lru);

		BUG_ON(!PageLRU(page));
		BUG_ON(PageActive(page));

		list_del(entry);
		list_add(entry, &inactive_list);

		/*
		 * Zero page counts can happen because we unlink the pages
		 * _after_ decrementing the usage count..
		 */
		if (unlikely(!page_count(page)))
			continue;

		if (!memclass(page_zone(page), classzone))
			continue;

		/* Racy check to avoid trylocking when not worthwhile */
		if (!page->buffers && (page_count(page) != 1 || !page->mapping))
			goto page_mapped;

		/*
		 * The page is locked. IO in progress?
		 * Move it to the back of the list.
		 */
		if (unlikely(TryLockPage(page))) {
			if (PageLaunder(page) && (gfp_mask & __GFP_FS)) {
				page_cache_get(page);
				spin_unlock(&pagemap_lru_lock);
				wait_on_page(page);
				page_cache_release(page);
				spin_lock(&pagemap_lru_lock);
			}
			continue;
		}

		if (PageDirty(page) && is_page_cache_freeable(page) && page->mapping) {
			/*
			 * It is not critical here to write it only if
			 * the page is unmapped beause any direct writer
			 * like O_DIRECT would set the PG_dirty bitflag
			 * on the phisical page after having successfully
			 * pinned it and after the I/O to the page is finished,
			 * so the direct writes to the page cannot get lost.
			 */
			int (*writepage)(struct page *);

			writepage = page->mapping->a_ops->writepage;
			if ((gfp_mask & __GFP_FS) && writepage) {
				ClearPageDirty(page);
				SetPageLaunder(page);
				page_cache_get(page);
				spin_unlock(&pagemap_lru_lock);

				writepage(page);
				page_cache_release(page);

				spin_lock(&pagemap_lru_lock);
				continue;
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 */
		if (page->buffers) {
			spin_unlock(&pagemap_lru_lock);

			/* avoid to free a locked page */
			page_cache_get(page);

			if (try_to_release_page(page, gfp_mask)) {
				if (!page->mapping) {
					/*
					 * We must not allow an anon page
					 * with no buffers to be visible on
					 * the LRU, so we unlock the page after
					 * taking the lru lock
					 */
					spin_lock(&pagemap_lru_lock);
					UnlockPage(page);
					__lru_cache_del(page);

					/* effectively free the page here */
					page_cache_release(page);

					if (--nr_pages)
						continue;
					break;
				} else {
					/*
					 * The page is still in pagecache so undo the stuff
					 * before the try_to_release_page since we've not
					 * finished and we can now try the next step.
					 */
					page_cache_release(page);

					spin_lock(&pagemap_lru_lock);
				}
			} else {
				/* failed to drop the buffers so stop here */
				UnlockPage(page);
				page_cache_release(page);

				spin_lock(&pagemap_lru_lock);
				continue;
			}
		}

		spin_lock(&pagecache_lock);

		/*
		 * this is the non-racy check for busy page.
		 */
		if (!page->mapping || !is_page_cache_freeable(page)) {
			spin_unlock(&pagecache_lock);
			UnlockPage(page);
page_mapped:
			if (--max_mapped >= 0)
				continue;

			/*
			 * Alert! We've found too many mapped pages on the
			 * inactive list, so we start swapping out now!
			 */
			spin_unlock(&pagemap_lru_lock);
			swap_out(priority, gfp_mask, classzone);
			return nr_pages;
		}

		/*
		 * It is critical to check PageDirty _after_ we made sure
		 * the page is freeable* so not in use by anybody.
		 */
		if (PageDirty(page)) {
			spin_unlock(&pagecache_lock);
			UnlockPage(page);
			continue;
		}

		/* point of no return */
		if (likely(!PageSwapCache(page))) {
			__remove_inode_page(page);
			spin_unlock(&pagecache_lock);
		} else {
			swp_entry_t swap;
			swap.val = page->index;
			__delete_from_swap_cache(page);
			spin_unlock(&pagecache_lock);
			swap_free(swap);
		}

		__lru_cache_del(page);
		UnlockPage(page);

		/* effectively free the page here */
		page_cache_release(page);

		if (--nr_pages)
			continue;
		break;
	}
	spin_unlock(&pagemap_lru_lock);

	return nr_pages;
}

/*
 * This moves pages from the active list to
 * the inactive list.
 *
 * We move them the other way when we see the
 * reference bit on the page.
 */
static void refill_inactive(int nr_pages)
{
	struct list_head * entry;

	spin_lock(&pagemap_lru_lock);
	entry = active_list.prev;
	while (nr_pages && entry != &active_list) {
		struct page * page;

		page = list_entry(entry, struct page, lru);
		entry = entry->prev;
		if (PageTestandClearReferenced(page)) {
			list_del(&page->lru);
			list_add(&page->lru, &active_list);
			continue;
		}

		nr_pages--;

		del_page_from_active_list(page);
		add_page_to_inactive_list(page);
		SetPageReferenced(page);
	}
	spin_unlock(&pagemap_lru_lock);
}

static int FASTCALL(shrink_caches(struct pm_zone * classzone, int priority, unsigned int gfp_mask, int nr_pages));
static int shrink_caches(struct pm_zone * classzone, int priority, unsigned int gfp_mask, int nr_pages)
{
	int chunk_size = nr_pages;
	unsigned long ratio;

	nr_pages -= kmem_cache_reap(gfp_mask);
	if (nr_pages <= 0)
		return 0;

	nr_pages = chunk_size;
	/* try to keep the active list 2/3 of the size of the cache */
	ratio = (unsigned long) nr_pages * nr_active_pages / ((nr_inactive_pages + 1) * 2);
	refill_inactive(ratio);

	nr_pages = shrink_cache(nr_pages, classzone, gfp_mask, priority);
	if (nr_pages <= 0)
		return 0;

	shrink_dcache_memory(priority, gfp_mask);
	shrink_icache_memory(priority, gfp_mask);
#ifdef CONFIG_QUOTA
	shrink_dqcache_memory(DEF_PRIORITY, gfp_mask);
#endif

	return nr_pages;
}

int try_to_free_pages_zone(struct pm_zone *classzone, unsigned int gfp_mask)
{
	int priority = DEF_PRIORITY;
	int nr_pages = SWAP_CLUSTER_MAX;

	gfp_mask = pf_gfp_mask(gfp_mask);
	do {
		nr_pages = shrink_caches(classzone, priority, gfp_mask, nr_pages);
		if (nr_pages <= 0)
			return 1;
	} while (--priority);

	/*
	 * Hmm.. Cache shrink failed - time to kill something?
	 * Mhwahahhaha! This is the part I really like. Giggle.
	 */
	out_of_memory();
	return 0;
}

int try_to_free_pages(unsigned int gfp_mask)
{
	struct pm_node *pmnod;
	zonelist_t *zonelist;
	unsigned long pf_free_pages;
	int error = 0;

	pf_free_pages = current->flags & PF_FREE_PAGES;
	current->flags &= ~PF_FREE_PAGES;

	for_each_pmnod(pmnod) {
		zonelist = pmnod->node_zonelists + (gfp_mask & GFP_ZONEMASK);
		error |= try_to_free_pages_zone(zonelist->zones[0], gfp_mask);
	}

	current->flags |= pf_free_pages;
	return error;
}

DECLARE_WAIT_QUEUE_HEAD(kswapd_wait);

static int check_classzone_need_balance(struct pm_zone * classzone)
{
	struct pm_zone * first_classzone;

	first_classzone = classzone->zone_pmnod->node_zones;
	while (classzone >= first_classzone) {
		if (classzone->free_pages > classzone->pages_high)
			return 0;
		classzone--;
	}
	return 1;
}

static int kswapd_balance_pmnod(struct pm_node * pmnod)
{
	int need_more_balance = 0, i;
	struct pm_zone * zone;

	for (i = pmnod->nr_zones-1; i >= 0; i--) {
		zone = pmnod->node_zones + i;
		if (unlikely(current->need_resched))
			schedule();
		if (!zone->need_balance)
			continue;
		if (!try_to_free_pages_zone(zone, GFP_KSWAPD)) {
			zone->need_balance = 0;
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}
		if (check_classzone_need_balance(zone))
			need_more_balance = 1;
		else
			zone->need_balance = 0;
	}

	return need_more_balance;
}

static void kswapd_balance(void)
{
	int need_more_balance;
	struct pm_node * pmnod;

	do {
		need_more_balance = 0;

		for_each_pmnod(pmnod)
			need_more_balance |= kswapd_balance_pmnod(pmnod);
	} while (need_more_balance);
}

static int kswapd_can_sleep_pmnod(struct pm_node * pmnod)
{
	struct pm_zone * zone;
	int i;

	for (i = pmnod->nr_zones-1; i >= 0; i--) {
		zone = pmnod->node_zones + i;
		if (!zone->need_balance)
			continue;
		return 0;
	}

	return 1;
}

static int kswapd_can_sleep(void)
{
	struct pm_node * pmnod;

	for_each_pmnod(pmnod) {
		if (!kswapd_can_sleep_pmnod(pmnod))
			return 0;
	}

	return 1;
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process. 
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
int kswapd(void *unused)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	daemonize();
	strcpy(tsk->comm, "kswapd");
	sigfillset(&tsk->blocked);
	
	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC;

	/*
	 * Kswapd main loop.
	 */
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&kswapd_wait, &wait);

		mb();
		if (kswapd_can_sleep())
			schedule();

		__set_current_state(TASK_RUNNING);
		remove_wait_queue(&kswapd_wait, &wait);

		/*
		 * If we actually get into a low-memory situation,
		 * the processes needing more memory will wake us
		 * up on a more timely basis.
		 */
		kswapd_balance();
		run_task_queue(&tq_disk);
	}
}

static int __init kswapd_init(void)
{
	printk("Starting kswapd\n");
	swap_setup();
	kernel_thread(kswapd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
	return 0;
}

module_init(kswapd_init)
