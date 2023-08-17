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

static struct bootmem contig_botm;
struct pm_node contig_pm_node = { botm: &contig_botm };

