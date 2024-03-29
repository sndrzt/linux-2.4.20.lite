/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (c) 1995,2001 Compaq Computer Corporation
 * Copyright (c) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (c) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/smp_lock.h>
#include <linux/pci.h>
#include "cpqphp.h"

static u32 configure_new_device(struct controller* ctrl, struct pci_func *func,u8 behind_bridge, struct resource_lists *resources);
static int configure_new_function(struct controller* ctrl, struct pci_func *func,u8 behind_bridge, struct resource_lists *resources);
static void interrupt_event_handler(struct controller *ctrl);

static struct semaphore event_semaphore;	/* mutex for process loop (up if something to process) */
static struct semaphore event_exit;		/* guard ensure thread has exited before calling it quits */
static int event_finished;
static unsigned long pushbutton_pending;	/* = 0 */

/* things needed for the long_delay function */
static struct semaphore		delay_sem;
static struct wait_queue_head_t	delay_wait;

/* delay is in jiffies to wait for */
static void long_delay (int delay)
{
	DECLARE_WAITQUEUE(wait, current);
	
	/* only allow 1 customer into the delay queue at once
	 * yes this makes some people wait even longer, but who really cares?
	 * this is for _huge_ delays to make the hardware happy as the 
	 * signals bounce around
	 */
	down (&delay_sem);

	init_waitqueue_head (&delay_wait);

	add_wait_queue(&delay_wait, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(delay);
	remove_wait_queue(&delay_wait, &wait);
	set_current_state(TASK_RUNNING);
	
	up (&delay_sem);
}


//FIXME: The following line needs to be somewhere else...
#define WRONG_BUS_FREQUENCY 0x07
static u8 handle_switch_change(u8 change, struct controller * ctrl)
{
	int hp_slot;
	u8 rc = 0;
	u16 temp_word;
	struct pci_func *func;
	struct event_info *taskInfo;

	if (!change)
		return 0;

	// Switch Change
	dbg("cpqsbd:  Switch interrupt received.\n");

	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x1L << hp_slot)) {
			//*********************************
			// this one changed.
			//*********************************
			func = cpqhp_slot_find(ctrl->bus, (hp_slot + ctrl->slot_device_offset), 0);

			//this is the structure that tells the worker thread
			//what to do
			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			taskInfo->hp_slot = hp_slot;

			rc++;

			temp_word = ctrl->ctrl_int_comp >> 16;
			func->presence_save = (temp_word >> hp_slot) & 0x01;
			func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

			if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
				//*********************************
				// Switch opened
				//*********************************

				func->switch_save = 0;

				taskInfo->event_type = INT_SWITCH_OPEN;
			} else {
				//*********************************
				// Switch closed
				//*********************************

				func->switch_save = 0x10;

				taskInfo->event_type = INT_SWITCH_CLOSE;
			}
		}
	}

	return rc;
}


/*
 * find_slot
 */
static inline struct slot *find_slot (struct controller * ctrl, u8 device)
{
	struct slot *slot;

	if (!ctrl)
		return NULL;

	slot = ctrl->slot;

	while (slot && (slot->device != device)) {
		slot = slot->next;
	}

	return slot;
}


static u8 handle_presence_change(u16 change, struct controller * ctrl)
{
	int hp_slot;
	u8 rc = 0;
	u8 temp_byte;
	u16 temp_word;
	struct pci_func *func;
	struct event_info *taskInfo;
	struct slot *p_slot;

	if (!change)
		return 0;

	//*********************************
	// Presence Change
	//*********************************
	dbg("cpqsbd:  Presence/Notify input change.\n");
	dbg("         Changed bits are 0x%4.4x\n", change );

	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x0101 << hp_slot)) {
			//*********************************
			// this one changed.
			//*********************************
			func = cpqhp_slot_find(ctrl->bus, (hp_slot + ctrl->slot_device_offset), 0);

			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			taskInfo->hp_slot = hp_slot;

			rc++;

			p_slot = find_slot(ctrl, hp_slot + (readb(ctrl->hpc_reg + SLOT_MASK) >> 4));

			// If the switch closed, must be a button
			// If not in button mode, nevermind
			if (func->switch_save && (ctrl->push_button == 1)) {
				temp_word = ctrl->ctrl_int_comp >> 16;
				temp_byte = (temp_word >> hp_slot) & 0x01;
				temp_byte |= (temp_word >> (hp_slot + 7)) & 0x02;

				if (temp_byte != func->presence_save) {
					//*********************************
					// button Pressed (doesn't do anything)
					//*********************************
					dbg("hp_slot %d button pressed\n", hp_slot);
					taskInfo->event_type = INT_BUTTON_PRESS;
				} else {
					//*********************************
					// button Released - TAKE ACTION!!!!
					//*********************************
					dbg("hp_slot %d button released\n", hp_slot);
					taskInfo->event_type = INT_BUTTON_RELEASE;

					// Cancel if we are still blinking
					if ((p_slot->state == BLINKINGON_STATE)
					    || (p_slot->state == BLINKINGOFF_STATE)) {
						taskInfo->event_type = INT_BUTTON_CANCEL;
						dbg("hp_slot %d button cancel\n", hp_slot);
					} else if ((p_slot->state == POWERON_STATE)
						   || (p_slot->state == POWEROFF_STATE)) {
						//info(msg_button_ignore, p_slot->number);
						taskInfo->event_type = INT_BUTTON_IGNORE;
						dbg("hp_slot %d button ignore\n", hp_slot);
					}
				}
			} else {
				// Switch is open, assume a presence change
				// Save the presence state
				temp_word = ctrl->ctrl_int_comp >> 16;
				func->presence_save = (temp_word >> hp_slot) & 0x01;
				func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

				if ((!(ctrl->ctrl_int_comp & (0x010000 << hp_slot))) ||
				    (!(ctrl->ctrl_int_comp & (0x01000000 << hp_slot)))) {
					//*********************************
					// Present
					//*********************************
					taskInfo->event_type = INT_PRESENCE_ON;
				} else {
					//*********************************
					// Not Present
					//*********************************
					taskInfo->event_type = INT_PRESENCE_OFF;
				}
			}
		}
	}

	return rc;
}


static u8 handle_power_fault(u8 change, struct controller * ctrl)
{
	int hp_slot;
	u8 rc = 0;
	struct pci_func *func;
	struct event_info *taskInfo;

	if (!change)
		return 0;

	//*********************************
	// power fault
	//*********************************

	info("power fault interrupt\n");

	for (hp_slot = 0; hp_slot < 6; hp_slot++) {
		if (change & (0x01 << hp_slot)) {
			//*********************************
			// this one changed.
			//*********************************
			func = cpqhp_slot_find(ctrl->bus, (hp_slot + ctrl->slot_device_offset), 0);

			taskInfo = &(ctrl->event_queue[ctrl->next_event]);
			ctrl->next_event = (ctrl->next_event + 1) % 10;
			taskInfo->hp_slot = hp_slot;

			rc++;

			if (ctrl->ctrl_int_comp & (0x00000100 << hp_slot)) {
				//*********************************
				// power fault Cleared
				//*********************************
				func->status = 0x00;

				taskInfo->event_type = INT_POWER_FAULT_CLEAR;
			} else {
				//*********************************
				// power fault
				//*********************************
				taskInfo->event_type = INT_POWER_FAULT;

				if (ctrl->rev < 4) {
					amber_LED_on (ctrl, hp_slot);
					green_LED_off (ctrl, hp_slot);
					set_SOGO (ctrl);

					// this is a fatal condition, we want to crash the
					// machine to protect from data corruption
					// simulated_NMI shouldn't ever return
					//FIXME
					//simulated_NMI(hp_slot, ctrl);

					//The following code causes a software crash just in
					//case simulated_NMI did return
					//FIXME
					//panic(msg_power_fault);
				} else {
					// set power fault status for this board
					func->status = 0xFF;
					info("power fault bit %x set\n", hp_slot);
				}
			}
		}
	}

	return rc;
}


/*
 * sort_by_size
 *
 * Sorts nodes on the list by their length.
 * Smallest first.
 *
 */
static int sort_by_size(struct pci_resource **head)
{
	struct pci_resource *current_res;
	struct pci_resource *next_res;
	int out_of_order = 1;

	if (!(*head))
		return(1);

	if (!((*head)->next))
		return(0);

	while (out_of_order) {
		out_of_order = 0;

		// Special case for swapping list head
		if (((*head)->next) &&
		    ((*head)->length > (*head)->next->length)) {
			out_of_order++;
			current_res = *head;
			*head = (*head)->next;
			current_res->next = (*head)->next;
			(*head)->next = current_res;
		}

		current_res = *head;

		while (current_res->next && current_res->next->next) {
			if (current_res->next->length > current_res->next->next->length) {
				out_of_order++;
				next_res = current_res->next;
				current_res->next = current_res->next->next;
				current_res = current_res->next;
				next_res->next = current_res->next;
				current_res->next = next_res;
			} else
				current_res = current_res->next;
		}
	}  // End of out_of_order loop

	return(0);
}


/*
 * sort_by_max_size
 *
 * Sorts nodes on the list by their length.
 * Largest first.
 *
 */
static int sort_by_max_size(struct pci_resource **head)
{
	struct pci_resource *current_res;
	struct pci_resource *next_res;
	int out_of_order = 1;

	if (!(*head))
		return(1);

	if (!((*head)->next))
		return(0);

	while (out_of_order) {
		out_of_order = 0;

		// Special case for swapping list head
		if (((*head)->next) &&
		    ((*head)->length < (*head)->next->length)) {
			out_of_order++;
			current_res = *head;
			*head = (*head)->next;
			current_res->next = (*head)->next;
			(*head)->next = current_res;
		}

		current_res = *head;

		while (current_res->next && current_res->next->next) {
			if (current_res->next->length < current_res->next->next->length) {
				out_of_order++;
				next_res = current_res->next;
				current_res->next = current_res->next->next;
				current_res = current_res->next;
				next_res->next = current_res->next;
				current_res->next = next_res;
			} else
				current_res = current_res->next;
		}
	}  // End of out_of_order loop

	return(0);
}


/*
 * do_pre_bridge_resource_split
 *
 *	Returns zero or one node of resources that aren't in use
 *
 */
static struct pci_resource *do_pre_bridge_resource_split (struct pci_resource **head, struct pci_resource **orig_head, u32 alignment)
{
	struct pci_resource *prevnode = NULL;
	struct pci_resource *node;
	struct pci_resource *split_node;
	u32 rc;
	u32 temp_dword;
	dbg("do_pre_bridge_resource_split\n");

	if (!(*head) || !(*orig_head))
		return(NULL);

	rc = cpqhp_resource_sort_and_combine(head);

	if (rc)
		return(NULL);

	if ((*head)->base != (*orig_head)->base)
		return(NULL);

	if ((*head)->length == (*orig_head)->length)
		return(NULL);


	// If we got here, there the bridge requires some of the resource, but
	// we may be able to split some off of the front

	node = *head;

	if (node->length & (alignment -1)) {
		// this one isn't an aligned length, so we'll make a new entry
		// and split it up.
		split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

		if (!split_node)
			return(NULL);

		temp_dword = (node->length | (alignment-1)) + 1 - alignment;

		split_node->base = node->base;
		split_node->length = temp_dword;

		node->length -= temp_dword;
		node->base += split_node->length;

		// Put it in the list
		*head = split_node;
		split_node->next = node;
	}

	if (node->length < alignment) {
		return(NULL);
	}

	// Now unlink it
	if (*head == node) {
		*head = node->next;
		node->next = NULL;
	} else {
		prevnode = *head;
		while (prevnode->next != node)
			prevnode = prevnode->next;

		prevnode->next = node->next;
		node->next = NULL;
	}

	return(node);
}


/*
 * do_bridge_resource_split
 *
 *	Returns zero or one node of resources that aren't in use
 *
 */
static struct pci_resource *do_bridge_resource_split (struct pci_resource **head, u32 alignment)
{
	struct pci_resource *prevnode = NULL;
	struct pci_resource *node;
	u32 rc;
	u32 temp_dword;

	if (!(*head))
		return(NULL);

	rc = cpqhp_resource_sort_and_combine(head);

	if (rc)
		return(NULL);

	node = *head;

	while (node->next) {
		prevnode = node;
		node = node->next;
		kfree(prevnode);
	}

	if (node->length < alignment) {
		kfree(node);
		return(NULL);
	}

	if (node->base & (alignment - 1)) {
		// Short circuit if adjusted size is too small
		temp_dword = (node->base | (alignment-1)) + 1;
		if ((node->length - (temp_dword - node->base)) < alignment) {
			kfree(node);
			return(NULL);
		}

		node->length -= (temp_dword - node->base);
		node->base = temp_dword;
	}

	if (node->length & (alignment - 1)) {
		// There's stuff in use after this node
		kfree(node);
		return(NULL);
	}

	return(node);
}


/*
 * get_io_resource
 *
 * this function sorts the resource list by size and then
 * returns the first node of "size" length that is not in the
 * ISA aliasing window.  If it finds a node larger than "size"
 * it will split it up.
 *
 * size must be a power of two.
 */
static struct pci_resource *get_io_resource (struct pci_resource **head, u32 size)
{
	struct pci_resource *prevnode;
	struct pci_resource *node;
	struct pci_resource *split_node;
	u32 temp_dword;

	if (!(*head))
		return(NULL);

	if ( cpqhp_resource_sort_and_combine(head) )
		return(NULL);

	if ( sort_by_size(head) )
		return(NULL);

	for (node = *head; node; node = node->next) {
		if (node->length < size)
			continue;

		if (node->base & (size - 1)) {
			// this one isn't base aligned properly
			// so we'll make a new entry and split it up
			temp_dword = (node->base | (size-1)) + 1;

			// Short circuit if adjusted size is too small
			if ((node->length - (temp_dword - node->base)) < size)
				continue;

			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);

			split_node->base = node->base;
			split_node->length = temp_dword - node->base;
			node->base = temp_dword;
			node->length -= split_node->length;

			// Put it in the list
			split_node->next = node->next;
			node->next = split_node;
		} // End of non-aligned base

		// Don't need to check if too small since we already did
		if (node->length > size) {
			// this one is longer than we need
			// so we'll make a new entry and split it up
			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);

			split_node->base = node->base + size;
			split_node->length = node->length - size;
			node->length = size;

			// Put it in the list
			split_node->next = node->next;
			node->next = split_node;
		}  // End of too big on top end

		// For IO make sure it's not in the ISA aliasing space
		if (node->base & 0x300L)
			continue;

		// If we got here, then it is the right size
		// Now take it out of the list
		if (*head == node) {
			*head = node->next;
		} else {
			prevnode = *head;
			while (prevnode->next != node)
				prevnode = prevnode->next;

			prevnode->next = node->next;
		}
		node->next = NULL;
		// Stop looping
		break;
	}

	return(node);
}


/*
 * get_max_resource
 *
 * Gets the largest node that is at least "size" big from the
 * list pointed to by head.  It aligns the node on top and bottom
 * to "size" alignment before returning it.
 */
static struct pci_resource *get_max_resource (struct pci_resource **head, u32 size)
{
	struct pci_resource *max;
	struct pci_resource *temp;
	struct pci_resource *split_node;
	u32 temp_dword;

	if (!(*head))
		return(NULL);

	if (cpqhp_resource_sort_and_combine(head))
		return(NULL);

	if (sort_by_max_size(head))
		return(NULL);

	for (max = *head;max; max = max->next) {

		// If not big enough we could probably just bail, 
		// instead we'll continue to the next.
		if (max->length < size)
			continue;

		if (max->base & (size - 1)) {
			// this one isn't base aligned properly
			// so we'll make a new entry and split it up
			temp_dword = (max->base | (size-1)) + 1;

			// Short circuit if adjusted size is too small
			if ((max->length - (temp_dword - max->base)) < size)
				continue;

			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);

			split_node->base = max->base;
			split_node->length = temp_dword - max->base;
			max->base = temp_dword;
			max->length -= split_node->length;

			// Put it next in the list
			split_node->next = max->next;
			max->next = split_node;
		}

		if ((max->base + max->length) & (size - 1)) {
			// this one isn't end aligned properly at the top
			// so we'll make a new entry and split it up
			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);
			temp_dword = ((max->base + max->length) & ~(size - 1));
			split_node->base = temp_dword;
			split_node->length = max->length + max->base
					     - split_node->base;
			max->length -= split_node->length;

			// Put it in the list
			split_node->next = max->next;
			max->next = split_node;
		}

		// Make sure it didn't shrink too much when we aligned it
		if (max->length < size)
			continue;

		// Now take it out of the list
		temp = (struct pci_resource*) *head;
		if (temp == max) {
			*head = max->next;
		} else {
			while (temp && temp->next != max) {
				temp = temp->next;
			}

			temp->next = max->next;
		}

		max->next = NULL;
		return(max);
	}

	// If we get here, we couldn't find one
	return(NULL);
}


/*
 * get_resource
 *
 * this function sorts the resource list by size and then
 * returns the first node of "size" length.  If it finds a node
 * larger than "size" it will split it up.
 *
 * size must be a power of two.
 */
static struct pci_resource *get_resource (struct pci_resource **head, u32 size)
{
	struct pci_resource *prevnode;
	struct pci_resource *node;
	struct pci_resource *split_node;
	u32 temp_dword;

	if (!(*head))
		return(NULL);

	if ( cpqhp_resource_sort_and_combine(head) )
		return(NULL);

	if ( sort_by_size(head) )
		return(NULL);

	for (node = *head; node; node = node->next) {
		dbg(__FUNCTION__": req_size =%x node=%p, base=%x, length=%x\n",
		    size, node, node->base, node->length);
		if (node->length < size)
			continue;

		if (node->base & (size - 1)) {
			dbg(__FUNCTION__": not aligned\n");
			// this one isn't base aligned properly
			// so we'll make a new entry and split it up
			temp_dword = (node->base | (size-1)) + 1;

			// Short circuit if adjusted size is too small
			if ((node->length - (temp_dword - node->base)) < size)
				continue;

			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);

			split_node->base = node->base;
			split_node->length = temp_dword - node->base;
			node->base = temp_dword;
			node->length -= split_node->length;

			// Put it in the list
			split_node->next = node->next;
			node->next = split_node;
		} // End of non-aligned base

		// Don't need to check if too small since we already did
		if (node->length > size) {
			dbg(__FUNCTION__": too big\n");
			// this one is longer than we need
			// so we'll make a new entry and split it up
			split_node = (struct pci_resource*) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

			if (!split_node)
				return(NULL);

			split_node->base = node->base + size;
			split_node->length = node->length - size;
			node->length = size;

			// Put it in the list
			split_node->next = node->next;
			node->next = split_node;
		}  // End of too big on top end

		dbg(__FUNCTION__": got one!!!\n");
		// If we got here, then it is the right size
		// Now take it out of the list
		if (*head == node) {
			*head = node->next;
		} else {
			prevnode = *head;
			while (prevnode->next != node)
				prevnode = prevnode->next;

			prevnode->next = node->next;
		}
		node->next = NULL;
		// Stop looping
		break;
	}
	return(node);
}


/*
 * cpqhp_resource_sort_and_combine
 *
 * Sorts all of the nodes in the list in ascending order by
 * their base addresses.  Also does garbage collection by
 * combining adjacent nodes.
 *
 * returns 0 if success
 */
int cpqhp_resource_sort_and_combine(struct pci_resource **head)
{
	struct pci_resource *node1;
	struct pci_resource *node2;
	int out_of_order = 1;

	dbg(__FUNCTION__": head = %p, *head = %p\n", head, *head);

	if (!(*head))
		return(1);

	dbg("*head->next = %p\n",(*head)->next);

	if (!(*head)->next)
		return(0);	/* only one item on the list, already sorted! */

	dbg("*head->base = 0x%x\n",(*head)->base);
	dbg("*head->next->base = 0x%x\n",(*head)->next->base);
	while (out_of_order) {
		out_of_order = 0;

		// Special case for swapping list head
		if (((*head)->next) &&
		    ((*head)->base > (*head)->next->base)) {
			node1 = *head;
			(*head) = (*head)->next;
			node1->next = (*head)->next;
			(*head)->next = node1;
			out_of_order++;
		}

		node1 = (*head);

		while (node1->next && node1->next->next) {
			if (node1->next->base > node1->next->next->base) {
				out_of_order++;
				node2 = node1->next;
				node1->next = node1->next->next;
				node1 = node1->next;
				node2->next = node1->next;
				node1->next = node2;
			} else
				node1 = node1->next;
		}
	}  // End of out_of_order loop

	node1 = *head;

	while (node1 && node1->next) {
		if ((node1->base + node1->length) == node1->next->base) {
			// Combine
			dbg("8..\n");
			node1->length += node1->next->length;
			node2 = node1->next;
			node1->next = node1->next->next;
			kfree(node2);
		} else
			node1 = node1->next;
	}

	return(0);
}


void cpqhp_ctrl_intr(int IRQ, struct controller * ctrl, struct pt_regs *regs)
{
	u8 schedule_flag = 0;
	u16 misc;
	u32 Diff;
	u32 temp_dword;

	
	misc = readw(ctrl->hpc_reg + MISC);
	//*********************************
	// Check to see if it was our interrupt
	//*********************************
	if (!(misc & 0x000C)) {
		return;
	}

	if (misc & 0x0004) {
		//*********************************
		// Serial Output interrupt Pending
		//*********************************

		// Clear the interrupt
		misc |= 0x0004;
		writew(misc, ctrl->hpc_reg + MISC);

		// Read to clear posted writes
		misc = readw(ctrl->hpc_reg + MISC);

		dbg (__FUNCTION__" - waking up\n");
		wake_up_interruptible(&ctrl->queue);
	}

	if (misc & 0x0008) {
		// General-interrupt-input interrupt Pending
		Diff = readl(ctrl->hpc_reg + INT_INPUT_CLEAR) ^ ctrl->ctrl_int_comp;

		ctrl->ctrl_int_comp = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

		// Clear the interrupt
		writel(Diff, ctrl->hpc_reg + INT_INPUT_CLEAR);

		// Read it back to clear any posted writes
		temp_dword = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

		if (!Diff) {
			// Clear all interrupts
			writel(0xFFFFFFFF, ctrl->hpc_reg + INT_INPUT_CLEAR);
		}

		schedule_flag += handle_switch_change((u8)(Diff & 0xFFL), ctrl);
		schedule_flag += handle_presence_change((u16)((Diff & 0xFFFF0000L) >> 16), ctrl);
		schedule_flag += handle_power_fault((u8)((Diff & 0xFF00L) >> 8), ctrl);
	}

	if (schedule_flag) {
		up(&event_semaphore);
		dbg("Signal event_semaphore\n");
		mark_bh(IMMEDIATE_BH);
	}

}


/**
 * cpqhp_slot_create - Creates a node and adds it to the proper bus.
 * @busnumber - bus where new node is to be located
 *
 * Returns pointer to the new node or NULL if unsuccessful
 */
struct pci_func *cpqhp_slot_create(u8 busnumber)
{
	struct pci_func *new_slot;
	struct pci_func *next;

	new_slot = (struct pci_func *) kmalloc(sizeof(struct pci_func), GFP_KERNEL);

	if (new_slot == NULL) {
		// I'm not dead yet!
		// You will be.
		return(new_slot);
	}

	memset(new_slot, 0, sizeof(struct pci_func));

	new_slot->next = NULL;
	new_slot->configured = 1;

	if (cpqhp_slot_list[busnumber] == NULL) {
		cpqhp_slot_list[busnumber] = new_slot;
	} else {
		next = cpqhp_slot_list[busnumber];
		while (next->next != NULL)
			next = next->next;
		next->next = new_slot;
	}
	return(new_slot);
}


/*
 * slot_remove - Removes a node from the linked list of slots.
 * @old_slot: slot to remove
 *
 * Returns 0 if successful, !0 otherwise.
 */
static int slot_remove(struct pci_func * old_slot)
{
	struct pci_func *next;

	if (old_slot == NULL)
		return(1);

	next = cpqhp_slot_list[old_slot->bus];

	if (next == NULL) {
		return(1);
	}

	if (next == old_slot) {
		cpqhp_slot_list[old_slot->bus] = old_slot->next;
		cpqhp_destroy_board_resources(old_slot);
		kfree(old_slot);
		return(0);
	}

	while ((next->next != old_slot) && (next->next != NULL)) {
		next = next->next;
	}

	if (next->next == old_slot) {
		next->next = old_slot->next;
		cpqhp_destroy_board_resources(old_slot);
		kfree(old_slot);
		return(0);
	} else
		return(2);
}


/**
 * bridge_slot_remove - Removes a node from the linked list of slots.
 * @bridge: bridge to remove
 *
 * Returns 0 if successful, !0 otherwise.
 */
static int bridge_slot_remove(struct pci_func *bridge)
{
	u8 subordinateBus, secondaryBus;
	u8 tempBus;
	struct pci_func *next;

	if (bridge == NULL)
		return(1);

	secondaryBus = (bridge->config_space[0x06] >> 8) & 0xFF;
	subordinateBus = (bridge->config_space[0x06] >> 16) & 0xFF;

	for (tempBus = secondaryBus; tempBus <= subordinateBus; tempBus++) {
		next = cpqhp_slot_list[tempBus];

		while (!slot_remove(next)) {
			next = cpqhp_slot_list[tempBus];
		}
	}

	next = cpqhp_slot_list[bridge->bus];

	if (next == NULL) {
		return(1);
	}

	if (next == bridge) {
		cpqhp_slot_list[bridge->bus] = bridge->next;
		kfree(bridge);
		return(0);
	}

	while ((next->next != bridge) && (next->next != NULL)) {
		next = next->next;
	}

	if (next->next == bridge) {
		next->next = bridge->next;
		kfree(bridge);
		return(0);
	} else
		return(2);
}


/**
 * cpqhp_slot_find - Looks for a node by bus, and device, multiple functions accessed
 * @bus: bus to find
 * @device: device to find
 * @index: is 0 for first function found, 1 for the second...
 *
 * Returns pointer to the node if successful, %NULL otherwise.
 */
struct pci_func *cpqhp_slot_find(u8 bus, u8 device, u8 index)
{
	int found = -1;
	struct pci_func *func;

	func = cpqhp_slot_list[bus];

	if ((func == NULL) || ((func->device == device) && (index == 0)))
		return(func);

	if (func->device == device)
		found++;

	while (func->next != NULL) {
		func = func->next;

		if (func->device == device)
			found++;

		if (found == index)
			return(func);
	}

	return(NULL);
}


// DJZ: I don't think is_bridge will work as is.
//FIXME
static int is_bridge(struct pci_func * func)
{
	// Check the header type
	if (((func->config_space[0x03] >> 16) & 0xFF) == 0x01)
		return 1;
	else
		return 0;
}


/* the following routines constitute the bulk of the 
   hotplug controller logic
 */


/**
 * board_replaced - Called after a board has been replaced in the system.
 *
 * This is only used if we don't have resources for hot add
 * Turns power on for the board
 * Checks to see if board is the same
 * If board is same, reconfigures it
 * If board isn't same, turns it back off.
 *
 */
static u32 board_replaced(struct pci_func * func, struct controller * ctrl)
{
	u8 hp_slot;
	u8 temp_byte;
	u32 index;
	u32 rc = 0;
	u32 src = 8;

	hp_slot = func->device - ctrl->slot_device_offset;

	if (readl(ctrl->hpc_reg + INT_INPUT_CLEAR) & (0x01L << hp_slot)) {
		//*********************************
		// The switch is open.
		//*********************************
		rc = INTERLOCK_OPEN;
	} else if (is_slot_enabled (ctrl, hp_slot)) {
		//*********************************
		// The board is already on
		//*********************************
		rc = CARD_FUNCTIONING;
	} else {
		if (ctrl->speed == PCI_SPEED_66MHz) {
			// Wait for exclusive access to hardware
			down(&ctrl->crit_sect);

			// turn on board without attaching to the bus
			enable_slot_power (ctrl, hp_slot);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			// Change bits in slot power register to force another shift out
			// NOTE: this is to work around the timer bug
			temp_byte = readb(ctrl->hpc_reg + SLOT_POWER);
			writeb(0x00, ctrl->hpc_reg + SLOT_POWER);
			writeb(temp_byte, ctrl->hpc_reg + SLOT_POWER);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			if (!(readl(ctrl->hpc_reg + NON_INT_INPUT) & (0x01 << hp_slot))) {
				rc = WRONG_BUS_FREQUENCY;
			}
			// turn off board without attaching to the bus
			disable_slot_power (ctrl, hp_slot);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			// Done with exclusive hardware access
			up(&ctrl->crit_sect);

			if (rc)
				return(rc);
		}

		// Wait for exclusive access to hardware
		down(&ctrl->crit_sect);

		slot_enable (ctrl, hp_slot);
		green_LED_blink (ctrl, hp_slot);

		amber_LED_off (ctrl, hp_slot);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		// Done with exclusive hardware access
		up(&ctrl->crit_sect);

		// Wait for ~1 second because of hot plug spec
		long_delay(1*HZ);

		// Check for a power fault
		if (func->status == 0xFF) {
			// power fault occurred, but it was benign
			rc = POWER_FAILURE;
			func->status = 0;
		} else
			rc = cpqhp_valid_replace(ctrl, func);

		if (!rc) {
			// It must be the same board

			rc = cpqhp_configure_board(ctrl, func);

			if (rc || src) {
				// If configuration fails, turn it off
				// Get slot won't work for devices behind bridges, but
				// in this case it will always be called for the "base"
				// bus/dev/func of an adapter.

				// Wait for exclusive access to hardware
				down(&ctrl->crit_sect);

				amber_LED_on (ctrl, hp_slot);
				green_LED_off (ctrl, hp_slot);
				slot_disable (ctrl, hp_slot);

				set_SOGO(ctrl);

				// Wait for SOBS to be unset
				wait_for_ctrl_irq (ctrl);

				// Done with exclusive hardware access
				up(&ctrl->crit_sect);

				if (rc)
					return(rc);
				else
					return(1);
			}

			func->status = 0;
			func->switch_save = 0x10;

			index = 1;
			while (((func = cpqhp_slot_find(func->bus, func->device, index)) != NULL) && !rc) {
				rc |= cpqhp_configure_board(ctrl, func);
				index++;
			}

			if (rc) {
				// If configuration fails, turn it off
				// Get slot won't work for devices behind bridges, but
				// in this case it will always be called for the "base"
				// bus/dev/func of an adapter.

				// Wait for exclusive access to hardware
				down(&ctrl->crit_sect);

				amber_LED_on (ctrl, hp_slot);
				green_LED_off (ctrl, hp_slot);
				slot_disable (ctrl, hp_slot);

				set_SOGO(ctrl);

				// Wait for SOBS to be unset
				wait_for_ctrl_irq (ctrl);

				// Done with exclusive hardware access
				up(&ctrl->crit_sect);

				return(rc);
			}
			// Done configuring so turn LED on full time

			// Wait for exclusive access to hardware
			down(&ctrl->crit_sect);

			green_LED_on (ctrl, hp_slot);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			// Done with exclusive hardware access
			up(&ctrl->crit_sect);
			rc = 0;
		} else {
			// Something is wrong

			// Get slot won't work for devices behind bridges, but
			// in this case it will always be called for the "base"
			// bus/dev/func of an adapter.

			// Wait for exclusive access to hardware
			down(&ctrl->crit_sect);

			amber_LED_on (ctrl, hp_slot);
			green_LED_off (ctrl, hp_slot);
			slot_disable (ctrl, hp_slot);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			// Done with exclusive hardware access
			up(&ctrl->crit_sect);
		}

	}
	return(rc);

}


/**
 * board_added - Called after a board has been added to the system.
 *
 * Turns power on for the board
 * Configures board
 *
 */
static u32 board_added(struct pci_func * func, struct controller * ctrl)
{
	u8 hp_slot;
	u8 temp_byte;
	int index;
	u32 temp_register = 0xFFFFFFFF;
	u32 rc = 0;
	struct pci_func *new_slot = NULL;
	struct slot *p_slot;
	struct resource_lists res_lists;

	hp_slot = func->device - ctrl->slot_device_offset;
	dbg(__FUNCTION__": func->device, slot_offset, hp_slot = %d, %d ,%d\n",
	    func->device, ctrl->slot_device_offset, hp_slot);

	if (ctrl->speed == PCI_SPEED_66MHz) {
		// Wait for exclusive access to hardware
		down(&ctrl->crit_sect);

		// turn on board without attaching to the bus
		enable_slot_power (ctrl, hp_slot);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		// Change bits in slot power register to force another shift out
		// NOTE: this is to work around the timer bug
		temp_byte = readb(ctrl->hpc_reg + SLOT_POWER);
		writeb(0x00, ctrl->hpc_reg + SLOT_POWER);
		writeb(temp_byte, ctrl->hpc_reg + SLOT_POWER);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		if (!(readl(ctrl->hpc_reg + NON_INT_INPUT) & (0x01 << hp_slot))) {
			rc = WRONG_BUS_FREQUENCY;
		}
		// turn off board without attaching to the bus
		disable_slot_power (ctrl, hp_slot);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		// Done with exclusive hardware access
		up(&ctrl->crit_sect);

		if (rc)
			return(rc);
	}
	p_slot = find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

	// turn on board and blink green LED

	// Wait for exclusive access to hardware
	dbg(__FUNCTION__": before down\n");
	down(&ctrl->crit_sect);
	dbg(__FUNCTION__": after down\n");

	dbg(__FUNCTION__": before slot_enable\n");
	slot_enable (ctrl, hp_slot);

	dbg(__FUNCTION__": before green_LED_blink\n");
	green_LED_blink (ctrl, hp_slot);

	dbg(__FUNCTION__": before amber_LED_blink\n");
	amber_LED_off (ctrl, hp_slot);

	dbg(__FUNCTION__": before set_SOGO\n");
	set_SOGO(ctrl);

	// Wait for SOBS to be unset
	dbg(__FUNCTION__": before wait_for_ctrl_irq\n");
	wait_for_ctrl_irq (ctrl);
	dbg(__FUNCTION__": after wait_for_ctrl_irq\n");

	// Done with exclusive hardware access
	dbg(__FUNCTION__": before up\n");
	up(&ctrl->crit_sect);
	dbg(__FUNCTION__": after up\n");

	// Wait for ~1 second because of hot plug spec
	dbg(__FUNCTION__": before long_delay\n");
	long_delay(1*HZ);
	dbg(__FUNCTION__": after long_delay\n");

	dbg(__FUNCTION__": func status = %x\n", func->status);
	// Check for a power fault
	if (func->status == 0xFF) {
		// power fault occurred, but it was benign
		temp_register = 0xFFFFFFFF;
		dbg(__FUNCTION__": temp register set to %x by power fault\n", temp_register);
		rc = POWER_FAILURE;
		func->status = 0;
	} else {
		// Get vendor/device ID u32
		rc = pci_read_config_dword_nodev (ctrl->pci_ops, func->bus, func->device, func->function, PCI_VENDOR_ID, &temp_register);
		dbg(__FUNCTION__": pci_read_config_dword returns %d\n", rc);
		dbg(__FUNCTION__": temp_register is %x\n", temp_register);

		if (rc != 0) {
			// Something's wrong here
			temp_register = 0xFFFFFFFF;
			dbg(__FUNCTION__": temp register set to %x by error\n", temp_register);
		}
		// Preset return code.  It will be changed later if things go okay.
		rc = NO_ADAPTER_PRESENT;
	}

	// All F's is an empty slot or an invalid board
	if (temp_register != 0xFFFFFFFF) {	  // Check for a board in the slot
		res_lists.io_head = ctrl->io_head;
		res_lists.mem_head = ctrl->mem_head;
		res_lists.p_mem_head = ctrl->p_mem_head;
		res_lists.bus_head = ctrl->bus_head;
		res_lists.irqs = NULL;

		rc = configure_new_device(ctrl, func, 0, &res_lists);

		dbg(__FUNCTION__": back from configure_new_device\n");
		ctrl->io_head = res_lists.io_head;
		ctrl->mem_head = res_lists.mem_head;
		ctrl->p_mem_head = res_lists.p_mem_head;
		ctrl->bus_head = res_lists.bus_head;

		cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
		cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
		cpqhp_resource_sort_and_combine(&(ctrl->io_head));
		cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

		if (rc) {
			// Wait for exclusive access to hardware
			down(&ctrl->crit_sect);

			amber_LED_on (ctrl, hp_slot);
			green_LED_off (ctrl, hp_slot);
			slot_disable (ctrl, hp_slot);

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);

			// Done with exclusive hardware access
			up(&ctrl->crit_sect);
			return(rc);
		} else {
			cpqhp_save_slot_config(ctrl, func);
		}


		func->status = 0;
		func->switch_save = 0x10;
		func->is_a_board = 0x01;

		//next, we will instantiate the linux pci_dev structures (with appropriate driver notification, if already present)
		dbg(__FUNCTION__": configure linux pci_dev structure\n");
		index = 0;
		do {
			new_slot = cpqhp_slot_find(ctrl->bus, func->device, index++);
			if (new_slot && !new_slot->pci_dev) {
				cpqhp_configure_device(ctrl, new_slot);
			}
		} while (new_slot);

		// Wait for exclusive access to hardware
		down(&ctrl->crit_sect);

		green_LED_on (ctrl, hp_slot);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		// Done with exclusive hardware access
		up(&ctrl->crit_sect);
	} else {
		// Wait for exclusive access to hardware
		down(&ctrl->crit_sect);

		amber_LED_on (ctrl, hp_slot);
		green_LED_off (ctrl, hp_slot);
		slot_disable (ctrl, hp_slot);

		set_SOGO(ctrl);

		// Wait for SOBS to be unset
		wait_for_ctrl_irq (ctrl);

		// Done with exclusive hardware access
		up(&ctrl->crit_sect);

		return(rc);
	}
	return 0;
}


/**
 * remove_board - Turns off slot and LED's
 *
 */
static u32 remove_board(struct pci_func * func, u32 replace_flag, struct controller * ctrl)
{
	int index;
	u8 skip = 0;
	u8 device;
	u8 hp_slot;
	u8 temp_byte;
	u32 rc;
	struct resource_lists res_lists;
	struct pci_func *temp_func;

	if (func == NULL)
		return(1);

	if (cpqhp_unconfigure_device(func))
		return(1);

	device = func->device;

	hp_slot = func->device - ctrl->slot_device_offset;
	dbg("In "__FUNCTION__", hp_slot = %d\n", hp_slot);

	// When we get here, it is safe to change base Address Registers.
	// We will attempt to save the base Address Register Lengths
	if (replace_flag || !ctrl->add_support)
		rc = cpqhp_save_base_addr_length(ctrl, func);
	else if (!func->bus_head && !func->mem_head &&
		 !func->p_mem_head && !func->io_head) {
		// Here we check to see if we've saved any of the board's
		// resources already.  If so, we'll skip the attempt to
		// determine what's being used.
		index = 0;
		temp_func = cpqhp_slot_find(func->bus, func->device, index++);
		while (temp_func) {
			if (temp_func->bus_head || temp_func->mem_head
			    || temp_func->p_mem_head || temp_func->io_head) {
				skip = 1;
				break;
			}
			temp_func = cpqhp_slot_find(temp_func->bus, temp_func->device, index++);
		}

		if (!skip)
			rc = cpqhp_save_used_resources(ctrl, func);
	}
	// Change status to shutdown
	if (func->is_a_board)
		func->status = 0x01;
	func->configured = 0;

	// Wait for exclusive access to hardware
	down(&ctrl->crit_sect);

	green_LED_off (ctrl, hp_slot);
	slot_disable (ctrl, hp_slot);

	set_SOGO(ctrl);

	// turn off SERR for slot
	temp_byte = readb(ctrl->hpc_reg + SLOT_SERR);
	temp_byte &= ~(0x01 << hp_slot);
	writeb(temp_byte, ctrl->hpc_reg + SLOT_SERR);

	// Wait for SOBS to be unset
	wait_for_ctrl_irq (ctrl);

	// Done with exclusive hardware access
	up(&ctrl->crit_sect);

	if (!replace_flag && ctrl->add_support) {
		while (func) {
			res_lists.io_head = ctrl->io_head;
			res_lists.mem_head = ctrl->mem_head;
			res_lists.p_mem_head = ctrl->p_mem_head;
			res_lists.bus_head = ctrl->bus_head;

			cpqhp_return_board_resources(func, &res_lists);

			ctrl->io_head = res_lists.io_head;
			ctrl->mem_head = res_lists.mem_head;
			ctrl->p_mem_head = res_lists.p_mem_head;
			ctrl->bus_head = res_lists.bus_head;

			cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
			cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
			cpqhp_resource_sort_and_combine(&(ctrl->io_head));
			cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

			if (is_bridge(func)) {
				bridge_slot_remove(func);
			} else
				slot_remove(func);

			func = cpqhp_slot_find(ctrl->bus, device, 0);
		}

		// Setup slot structure with entry for empty slot
		func = cpqhp_slot_create(ctrl->bus);

		if (func == NULL) {
			// Out of memory
			return(1);
		}

		func->bus = ctrl->bus;
		func->device = device;
		func->function = 0;
		func->configured = 0;
		func->switch_save = 0x10;
		func->is_a_board = 0;
		func->p_task_event = NULL;
	}

	return 0;
}


static void pushbutton_helper_thread (unsigned long data)
{
	pushbutton_pending = data;
	up(&event_semaphore);
}


// this is the main worker thread
static int event_thread(void* data)
{
	struct controller *ctrl;
	lock_kernel();
	daemonize();
	reparent_to_init();
	
	//  New name
	strcpy(current->comm, "phpd_event");
	
	unlock_kernel();

	while (1) {
		dbg("!!!!event_thread sleeping\n");
		down_interruptible (&event_semaphore);
		dbg("event_thread woken finished = %d\n", event_finished);
		if (event_finished) break;
		/* Do stuff here */
		if (pushbutton_pending)
			cpqhp_pushbutton_thread(pushbutton_pending);
		else
			for (ctrl = cpqhp_ctrl_list; ctrl; ctrl=ctrl->next)
				interrupt_event_handler(ctrl);
	}
	dbg("event_thread signals exit\n");
	up(&event_exit);
	return 0;
}


int cpqhp_event_start_thread (void)
{
	int pid;

	/* initialize our semaphores */
	init_MUTEX(&delay_sem);
	init_MUTEX_LOCKED(&event_semaphore);
	init_MUTEX_LOCKED(&event_exit);
	event_finished=0;

	pid = kernel_thread(event_thread, 0, 0);
	if (pid < 0) {
		err ("Can't start up our event thread\n");
		return -1;
	}
	dbg("Our event thread pid = %d\n", pid);
	return 0;
}


void cpqhp_event_stop_thread (void)
{
	event_finished = 1;
	dbg("event_thread finish command given\n");
	up(&event_semaphore);
	dbg("wait for event_thread to exit\n");
	down(&event_exit);
}


static int update_slot_info (struct controller *ctrl, struct slot *slot)
{
	struct hotplug_slot_info *info;
	char buffer[SLOT_NAME_SIZE];
	int result;

	info = kmalloc (sizeof (struct hotplug_slot_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	make_slot_name (&buffer[0], SLOT_NAME_SIZE, slot);
	info->power_status = get_slot_enabled(ctrl, slot);
	info->attention_status = cpq_get_attention_status(ctrl, slot);
	info->latch_status = cpq_get_latch_status(ctrl, slot);
	info->adapter_status = get_presence_status(ctrl, slot);
	result = pci_hp_change_slot_info(buffer, info);
	kfree (info);
	return result;
}

static void interrupt_event_handler(struct controller *ctrl)
{
	int loop = 0;
	int change = 1;
	struct pci_func *func;
	u8 hp_slot;
	struct slot *p_slot;

	while (change) {
		change = 0;

		for (loop = 0; loop < 10; loop++) {
			//dbg("loop %d\n", loop);
			if (ctrl->event_queue[loop].event_type != 0) {
				hp_slot = ctrl->event_queue[loop].hp_slot;

				func = cpqhp_slot_find(ctrl->bus, (hp_slot + ctrl->slot_device_offset), 0);

				p_slot = find_slot(ctrl, hp_slot + ctrl->slot_device_offset);

				dbg("hp_slot %d, func %p, p_slot %p\n",
				    hp_slot, func, p_slot);

				if (ctrl->event_queue[loop].event_type == INT_BUTTON_PRESS) {
					dbg("button pressed\n");
				} else if (ctrl->event_queue[loop].event_type == 
					   INT_BUTTON_CANCEL) {
					dbg("button cancel\n");
					del_timer(&p_slot->task_event);

					// Wait for exclusive access to hardware
					down(&ctrl->crit_sect);

					if (p_slot->state == BLINKINGOFF_STATE) {
						// slot is on
						// turn on green LED
						dbg("turn on green LED\n");
						green_LED_on (ctrl, hp_slot);
					} else if (p_slot->state == BLINKINGON_STATE) {
						// slot is off
						// turn off green LED
						dbg("turn off green LED\n");
						green_LED_off (ctrl, hp_slot);
					}

					info(msg_button_cancel, p_slot->number);

					p_slot->state = STATIC_STATE;

					amber_LED_off (ctrl, hp_slot);

					set_SOGO(ctrl);

					// Wait for SOBS to be unset
					wait_for_ctrl_irq (ctrl);

					// Done with exclusive hardware access
					up(&ctrl->crit_sect);
				}
				// ***********button Released (No action on press...)
				else if (ctrl->event_queue[loop].event_type == INT_BUTTON_RELEASE) {
					dbg("button release\n");

					if (is_slot_enabled (ctrl, hp_slot)) {
						// slot is on
						dbg("slot is on\n");
						p_slot->state = BLINKINGOFF_STATE;
						info(msg_button_off, p_slot->number);
					} else {
						// slot is off
						dbg("slot is off\n");
						p_slot->state = BLINKINGON_STATE;
						info(msg_button_on, p_slot->number);
					}
					// Wait for exclusive access to hardware
					down(&ctrl->crit_sect);

					dbg("blink green LED and turn off amber\n");
					amber_LED_off (ctrl, hp_slot);
					green_LED_blink (ctrl, hp_slot);

					set_SOGO(ctrl);

					// Wait for SOBS to be unset
					wait_for_ctrl_irq (ctrl);

					// Done with exclusive hardware access
					up(&ctrl->crit_sect);
					init_timer(&p_slot->task_event);
					p_slot->hp_slot = hp_slot;
					p_slot->ctrl = ctrl;
//					p_slot->physical_slot = physical_slot;
					p_slot->task_event.expires = jiffies + 5 * HZ;   // 5 second delay
					p_slot->task_event.function = pushbutton_helper_thread;
					p_slot->task_event.data = (u32) p_slot;

					dbg("add_timer p_slot = %p\n", p_slot);
					add_timer(&p_slot->task_event);
				}
				// ***********POWER FAULT
				else if (ctrl->event_queue[loop].event_type == INT_POWER_FAULT) {
					dbg("power fault\n");
				} else {
					/* refresh notification */
					if (p_slot)
						update_slot_info(ctrl, p_slot);
				}

				ctrl->event_queue[loop].event_type = 0;

				change = 1;
			}
		}		// End of FOR loop
	}

	return;
}


/**
 * cpqhp_pushbutton_thread
 *
 * Scheduled procedure to handle blocking stuff for the pushbuttons
 * Handles all pending events and exits.
 *
 */
void cpqhp_pushbutton_thread (unsigned long slot)
{
	u8 hp_slot;
	u8 device;
	struct pci_func *func;
	struct slot *p_slot = (struct slot *) slot;
	struct controller *ctrl = (struct controller *) p_slot->ctrl;

	pushbutton_pending = 0;
	hp_slot = p_slot->hp_slot;

	device = p_slot->device;

	if (is_slot_enabled (ctrl, hp_slot)) {
		p_slot->state = POWEROFF_STATE;
		// power Down board
		func = cpqhp_slot_find(p_slot->bus, p_slot->device, 0);
		dbg("In power_down_board, func = %p, ctrl = %p\n", func, ctrl);
		if (!func) {
			dbg("Error! func NULL in "__FUNCTION__"\n");
			return ;
		}

		if (func != NULL && ctrl != NULL) {
			if (cpqhp_process_SS(ctrl, func) != 0) {
				amber_LED_on (ctrl, hp_slot);
				green_LED_on (ctrl, hp_slot);
				
				set_SOGO(ctrl);

				// Wait for SOBS to be unset
				wait_for_ctrl_irq (ctrl);
			}
		}

		p_slot->state = STATIC_STATE;
	} else {
		p_slot->state = POWERON_STATE;
		// slot is off

		func = cpqhp_slot_find(p_slot->bus, p_slot->device, 0);
		dbg("In add_board, func = %p, ctrl = %p\n", func, ctrl);
		if (!func) {
			dbg("Error! func NULL in "__FUNCTION__"\n");
			return ;
		}

		if (func != NULL && ctrl != NULL) {
			if (cpqhp_process_SI(ctrl, func) != 0) {
				amber_LED_on (ctrl, hp_slot);
				green_LED_off (ctrl, hp_slot);
				
				set_SOGO(ctrl);

				// Wait for SOBS to be unset
				wait_for_ctrl_irq (ctrl);
			}
		}

		p_slot->state = STATIC_STATE;
	}

	return;
}


int cpqhp_process_SI (struct controller *ctrl, struct pci_func *func)
{
	u8 device, hp_slot;
	u16 temp_word;
	u32 tempdword;
	int rc;
	struct slot* p_slot;
	int physical_slot = 0;

	if (!ctrl)
		return(1);

	tempdword = 0;

	device = func->device;
	hp_slot = device - ctrl->slot_device_offset;
	p_slot = find_slot(ctrl, device);
	if (p_slot) {
		physical_slot = p_slot->number;
	}

	// Check to see if the interlock is closed
	tempdword = readl(ctrl->hpc_reg + INT_INPUT_CLEAR);

	if (tempdword & (0x01 << hp_slot)) {
		return(1);
	}

	if (func->is_a_board) {
		rc = board_replaced(func, ctrl);
	} else {
		// add board
		slot_remove(func);

		func = cpqhp_slot_create(ctrl->bus);
		if (func == NULL) {
			return(1);
		}

		func->bus = ctrl->bus;
		func->device = device;
		func->function = 0;
		func->configured = 0;
		func->is_a_board = 1;

		// We have to save the presence info for these slots
		temp_word = ctrl->ctrl_int_comp >> 16;
		func->presence_save = (temp_word >> hp_slot) & 0x01;
		func->presence_save |= (temp_word >> (hp_slot + 7)) & 0x02;

		if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
			func->switch_save = 0;
		} else {
			func->switch_save = 0x10;
		}

		rc = board_added(func, ctrl);
		if (rc) {
			if (is_bridge(func)) {
				bridge_slot_remove(func);
			} else
				slot_remove(func);

			// Setup slot structure with entry for empty slot
			func = cpqhp_slot_create(ctrl->bus);

			if (func == NULL) {
				// Out of memory
				return(1);
			}

			func->bus = ctrl->bus;
			func->device = device;
			func->function = 0;
			func->configured = 0;
			func->is_a_board = 0;

			// We have to save the presence info for these slots
			temp_word = ctrl->ctrl_int_comp >> 16;
			func->presence_save = (temp_word >> hp_slot) & 0x01;
			func->presence_save |=
			(temp_word >> (hp_slot + 7)) & 0x02;

			if (ctrl->ctrl_int_comp & (0x1L << hp_slot)) {
				func->switch_save = 0;
			} else {
				func->switch_save = 0x10;
			}
		}
	}

	if (rc) {
		dbg(__FUNCTION__": rc = %d\n", rc);
	}

	if (p_slot)
		update_slot_info(ctrl, p_slot);

	return rc;
}


int cpqhp_process_SS (struct controller *ctrl, struct pci_func *func)
{
	u8 device, class_code, header_type, BCR;
	u8 index = 0;
	u8 replace_flag;
	u32 rc = 0;
	struct slot* p_slot;
	int physical_slot=0;

	device = func->device; 
	func = cpqhp_slot_find(ctrl->bus, device, index++);
	p_slot = find_slot(ctrl, device);
	if (p_slot) {
		physical_slot = p_slot->number;
	}

	// Make sure there are no video controllers here
	while (func && !rc) {
		// Check the Class Code
		rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, 0x0B, &class_code);
		if (rc)
			return rc;

		if (class_code == PCI_BASE_CLASS_DISPLAY) {
			/* Display/Video adapter (not supported) */
			rc = REMOVE_NOT_SUPPORTED;
		} else {
			// See if it's a bridge
			rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, PCI_HEADER_TYPE, &header_type);
			if (rc)
				return rc;

			// If it's a bridge, check the VGA Enable bit
			if ((header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE) {
				rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, PCI_BRIDGE_CONTROL, &BCR);
				if (rc)
					return rc;

				// If the VGA Enable bit is set, remove isn't supported
				if (BCR & PCI_BRIDGE_CTL_VGA) {
					rc = REMOVE_NOT_SUPPORTED;
				}
			}
		}

		func = cpqhp_slot_find(ctrl->bus, device, index++);
	}

	func = cpqhp_slot_find(ctrl->bus, device, 0);
	if ((func != NULL) && !rc) {
		//FIXME: Replace flag should be passed into process_SS
		replace_flag = !(ctrl->add_support);
		rc = remove_board(func, replace_flag, ctrl);
	} else if (!rc) {
		rc = 1;
	}

	if (p_slot)
		update_slot_info(ctrl, p_slot);

	return(rc);
}



/**
 * hardware_test - runs hardware tests
 *
 * For hot plug ctrl folks to play with.
 * test_num is the number entered in the GUI
 *
 */
int cpqhp_hardware_test(struct controller *ctrl, int test_num)
{
	u32 save_LED;
	u32 work_LED;
	int loop;
	int num_of_slots;

	num_of_slots = readb(ctrl->hpc_reg + SLOT_MASK) & 0x0f;

	switch (test_num) {
		case 1:
			// Do stuff here!

			// Do that funky LED thing
			save_LED = readl(ctrl->hpc_reg + LED_CONTROL);	// so we can restore them later
			work_LED = 0x01010101;
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			for (loop = 0; loop < num_of_slots; loop++) {
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				work_LED = work_LED << 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				long_delay((2*HZ)/10);
			}
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED >> 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED << 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED >> 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}

			work_LED = 0x01010000;
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			for (loop = 0; loop < num_of_slots; loop++) {
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				work_LED = work_LED << 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				long_delay((2*HZ)/10);
			}
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED >> 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}
			work_LED = 0x00000101;
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED << 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}
			for (loop = 0; loop < num_of_slots; loop++) {
				work_LED = work_LED >> 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((2*HZ)/10);
			}


			work_LED = 0x01010000;
			writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			for (loop = 0; loop < num_of_slots; loop++) {
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((3*HZ)/10);
				work_LED = work_LED >> 16;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				
				set_SOGO(ctrl);

				// Wait for SOGO interrupt
				wait_for_ctrl_irq (ctrl);

				// Get ready for next iteration
				long_delay((3*HZ)/10);
				work_LED = work_LED << 16;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
				work_LED = work_LED << 1;
				writel(work_LED, ctrl->hpc_reg + LED_CONTROL);
			}

			writel (save_LED, ctrl->hpc_reg + LED_CONTROL);	// put it back the way it was

			set_SOGO(ctrl);

			// Wait for SOBS to be unset
			wait_for_ctrl_irq (ctrl);
			break;
		case 2:
			// Do other stuff here!
			break;
		case 3:
			// and more...
			break;
	}
	return 0;
}


/**
 * configure_new_device - Configures the PCI header information of one board.
 *
 * @ctrl: pointer to controller structure
 * @func: pointer to function structure
 * @behind_bridge: 1 if this is a recursive call, 0 if not
 * @resources: pointer to set of resource lists
 *
 * Returns 0 if success
 *
 */
static u32 configure_new_device (struct controller * ctrl, struct pci_func * func,
				 u8 behind_bridge, struct resource_lists * resources)
{
	u8 temp_byte, function, max_functions, stop_it;
	int rc;
	u32 ID;
	struct pci_func *new_slot;
	int index;

	new_slot = func;

	dbg(__FUNCTION__"\n");
	// Check for Multi-function device
	rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, 0x0E, &temp_byte);
	if (rc) {
		dbg(__FUNCTION__": rc = %d\n", rc);
		return rc;
	}

	if (temp_byte & 0x80)	// Multi-function device
		max_functions = 8;
	else
		max_functions = 1;

	function = 0;

	do {
		rc = configure_new_function(ctrl, new_slot, behind_bridge, resources);

		if (rc) {
			dbg("configure_new_function failed %d\n",rc);
			index = 0;

			while (new_slot) {
				new_slot = cpqhp_slot_find(new_slot->bus, new_slot->device, index++);

				if (new_slot)
					cpqhp_return_board_resources(new_slot, resources);
			}

			return(rc);
		}

		function++;

		stop_it = 0;

		//  The following loop skips to the next present function
		//  and creates a board structure

		while ((function < max_functions) && (!stop_it)) {
			pci_read_config_dword_nodev (ctrl->pci_ops, func->bus, func->device, function, 0x00, &ID);

			if (ID == 0xFFFFFFFF) {	  // There's nothing there. 
				function++;
			} else {  // There's something there
				// Setup slot structure.
				new_slot = cpqhp_slot_create(func->bus);

				if (new_slot == NULL) {
					// Out of memory
					return(1);
				}

				new_slot->bus = func->bus;
				new_slot->device = func->device;
				new_slot->function = function;
				new_slot->is_a_board = 1;
				new_slot->status = 0;

				stop_it++;
			}
		}

	} while (function < max_functions);
	dbg("returning from configure_new_device\n");

	return 0;
}


/*
  Configuration logic that involves the hotplug data structures and 
  their bookkeeping
 */


/**
 * configure_new_function - Configures the PCI header information of one device
 *
 * @ctrl: pointer to controller structure
 * @func: pointer to function structure
 * @behind_bridge: 1 if this is a recursive call, 0 if not
 * @resources: pointer to set of resource lists
 *
 * Calls itself recursively for bridged devices.
 * Returns 0 if success
 *
 */
static int configure_new_function (struct controller * ctrl, struct pci_func * func,
				   u8 behind_bridge, struct resource_lists * resources)
{
	int cloop;
	u8 IRQ;
	u8 temp_byte;
	u8 device;
	u8 class_code;
	u16 command;
	u16 temp_word;
	u32 temp_dword;
	u32 rc;
	u32 temp_register;
	u32 base;
	u32 ID;
	struct pci_resource *mem_node;
	struct pci_resource *p_mem_node;
	struct pci_resource *io_node;
	struct pci_resource *bus_node;
	struct pci_resource *hold_mem_node;
	struct pci_resource *hold_p_mem_node;
	struct pci_resource *hold_IO_node;
	struct pci_resource *hold_bus_node;
	struct irq_mapping irqs;
	struct pci_func *new_slot;
	struct resource_lists temp_resources;

	// Check for Bridge
	rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, PCI_HEADER_TYPE, &temp_byte);
	if (rc)
		return rc;

	if ((temp_byte & 0x7F) == PCI_HEADER_TYPE_BRIDGE) { // PCI-PCI Bridge
		// set Primary bus
		dbg("set Primary bus = %d\n", func->bus);
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PRIMARY_BUS, func->bus);
		if (rc)
			return rc;

		// find range of busses to use
		dbg("find ranges of buses to use\n");
		bus_node = get_max_resource(&resources->bus_head, 1);

		// If we don't have any busses to allocate, we can't continue
		if (!bus_node)
			return -ENOMEM;

		// set Secondary bus
		temp_byte = bus_node->base;
		dbg("set Secondary bus = %d\n", bus_node->base);
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_SECONDARY_BUS, temp_byte);
		if (rc)
			return rc;

		// set subordinate bus
		temp_byte = bus_node->base + bus_node->length - 1;
		dbg("set subordinate bus = %d\n", bus_node->base + bus_node->length - 1);
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_SUBORDINATE_BUS, temp_byte);
		if (rc)
			return rc;

		// set subordinate Latency Timer and base Latency Timer
		temp_byte = 0x40;
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_SEC_LATENCY_TIMER, temp_byte);
		if (rc)
			return rc;
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_LATENCY_TIMER, temp_byte);
		if (rc)
			return rc;

		// set Cache Line size
		temp_byte = 0x08;
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_CACHE_LINE_SIZE, temp_byte);
		if (rc)
			return rc;

		// Setup the IO, memory, and prefetchable windows

		io_node = get_max_resource(&(resources->io_head), 0x1000);
		mem_node = get_max_resource(&(resources->mem_head), 0x100000);
		p_mem_node = get_max_resource(&(resources->p_mem_head), 0x100000);
		dbg("Setup the IO, memory, and prefetchable windows\n");
		dbg("io_node\n");
		dbg("(base, len, next) (%x, %x, %p)\n", io_node->base, io_node->length, io_node->next);
		dbg("mem_node\n");
		dbg("(base, len, next) (%x, %x, %p)\n", mem_node->base, mem_node->length, mem_node->next);
		dbg("p_mem_node\n");
		dbg("(base, len, next) (%x, %x, %p)\n", p_mem_node->base, p_mem_node->length, p_mem_node->next);

		// set up the IRQ info
		if (!resources->irqs) {
			irqs.barber_pole = 0;
			irqs.interrupt[0] = 0;
			irqs.interrupt[1] = 0;
			irqs.interrupt[2] = 0;
			irqs.interrupt[3] = 0;
			irqs.valid_INT = 0;
		} else {
			irqs.barber_pole = resources->irqs->barber_pole;
			irqs.interrupt[0] = resources->irqs->interrupt[0];
			irqs.interrupt[1] = resources->irqs->interrupt[1];
			irqs.interrupt[2] = resources->irqs->interrupt[2];
			irqs.interrupt[3] = resources->irqs->interrupt[3];
			irqs.valid_INT = resources->irqs->valid_INT;
		}

		// set up resource lists that are now aligned on top and bottom
		// for anything behind the bridge.
		temp_resources.bus_head = bus_node;
		temp_resources.io_head = io_node;
		temp_resources.mem_head = mem_node;
		temp_resources.p_mem_head = p_mem_node;
		temp_resources.irqs = &irqs;

		// Make copies of the nodes we are going to pass down so that
		// if there is a problem,we can just use these to free resources
		hold_bus_node = (struct pci_resource *) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);
		hold_IO_node = (struct pci_resource *) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);
		hold_mem_node = (struct pci_resource *) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);
		hold_p_mem_node = (struct pci_resource *) kmalloc(sizeof(struct pci_resource), GFP_KERNEL);

		if (!hold_bus_node || !hold_IO_node || !hold_mem_node || !hold_p_mem_node) {
			if (hold_bus_node)
				kfree(hold_bus_node);
			if (hold_IO_node)
				kfree(hold_IO_node);
			if (hold_mem_node)
				kfree(hold_mem_node);
			if (hold_p_mem_node)
				kfree(hold_p_mem_node);

			return(1);
		}

		memcpy(hold_bus_node, bus_node, sizeof(struct pci_resource));

		bus_node->base += 1;
		bus_node->length -= 1;
		bus_node->next = NULL;

		// If we have IO resources copy them and fill in the bridge's
		// IO range registers
		if (io_node) {
			memcpy(hold_IO_node, io_node, sizeof(struct pci_resource));
			io_node->next = NULL;

			// set IO base and Limit registers
			temp_byte = io_node->base >> 8;
			rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_IO_BASE, temp_byte);

			temp_byte = (io_node->base + io_node->length - 1) >> 8;
			rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_IO_LIMIT, temp_byte);
		} else {
			kfree(hold_IO_node);
			hold_IO_node = NULL;
		}

		// If we have memory resources copy them and fill in the bridge's
		// memory range registers.  Otherwise, fill in the range
		// registers with values that disable them.
		if (mem_node) {
			memcpy(hold_mem_node, mem_node, sizeof(struct pci_resource));
			mem_node->next = NULL;

			// set Mem base and Limit registers
			temp_word = mem_node->base >> 16;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_BASE, temp_word);

			temp_word = (mem_node->base + mem_node->length - 1) >> 16;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_LIMIT, temp_word);
		} else {
			temp_word = 0xFFFF;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_BASE, temp_word);

			temp_word = 0x0000;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_LIMIT, temp_word);

			kfree(hold_mem_node);
			hold_mem_node = NULL;
		}

		// If we have prefetchable memory resources copy them and 
		// fill in the bridge's memory range registers.  Otherwise,
		// fill in the range registers with values that disable them.
		if (p_mem_node) {
			memcpy(hold_p_mem_node, p_mem_node, sizeof(struct pci_resource));
			p_mem_node->next = NULL;

			// set Pre Mem base and Limit registers
			temp_word = p_mem_node->base >> 16;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_BASE, temp_word);

			temp_word = (p_mem_node->base + p_mem_node->length - 1) >> 16;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_LIMIT, temp_word);
		} else {
			temp_word = 0xFFFF;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_BASE, temp_word);

			temp_word = 0x0000;
			rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_LIMIT, temp_word);

			kfree(hold_p_mem_node);
			hold_p_mem_node = NULL;
		}

		// Adjust this to compensate for extra adjustment in first loop
		irqs.barber_pole--;

		rc = 0;

		// Here we actually find the devices and configure them
		for (device = 0; (device <= 0x1F) && !rc; device++) {
			irqs.barber_pole = (irqs.barber_pole + 1) & 0x03;

			ID = 0xFFFFFFFF;
			pci_read_config_dword_nodev (ctrl->pci_ops, hold_bus_node->base, device, 0, 0x00, &ID);

			if (ID != 0xFFFFFFFF) {	  //  device Present
				// Setup slot structure.
				new_slot = cpqhp_slot_create(hold_bus_node->base);

				if (new_slot == NULL) {
					// Out of memory
					rc = -ENOMEM;
					continue;
				}

				new_slot->bus = hold_bus_node->base;
				new_slot->device = device;
				new_slot->function = 0;
				new_slot->is_a_board = 1;
				new_slot->status = 0;

				rc = configure_new_device(ctrl, new_slot, 1, &temp_resources);
				dbg("configure_new_device rc=0x%x\n",rc);
			}	// End of IF (device in slot?)
		}		// End of FOR loop

		if (rc) {
			cpqhp_destroy_resource_list(&temp_resources);

			return_resource(&(resources->bus_head), hold_bus_node);
			return_resource(&(resources->io_head), hold_IO_node);
			return_resource(&(resources->mem_head), hold_mem_node);
			return_resource(&(resources->p_mem_head), hold_p_mem_node);
			return(rc);
		}
		// save the interrupt routing information
		if (resources->irqs) {
			resources->irqs->interrupt[0] = irqs.interrupt[0];
			resources->irqs->interrupt[1] = irqs.interrupt[1];
			resources->irqs->interrupt[2] = irqs.interrupt[2];
			resources->irqs->interrupt[3] = irqs.interrupt[3];
			resources->irqs->valid_INT = irqs.valid_INT;
		} else if (!behind_bridge) {
			// We need to hook up the interrupts here
			for (cloop = 0; cloop < 4; cloop++) {
				if (irqs.valid_INT & (0x01 << cloop)) {
					rc = cpqhp_set_irq(func->bus, func->device,
							   0x0A + cloop, irqs.interrupt[cloop]);
					if (rc) {
						cpqhp_destroy_resource_list (&temp_resources);

						return_resource(&(resources-> bus_head), hold_bus_node);
						return_resource(&(resources-> io_head), hold_IO_node);
						return_resource(&(resources-> mem_head), hold_mem_node);
						return_resource(&(resources-> p_mem_head), hold_p_mem_node);
						return rc;
					}
				}
			}	// end of for loop
		}
		// Return unused bus resources
		// First use the temporary node to store information for the board
		if (hold_bus_node && bus_node && temp_resources.bus_head) {
			hold_bus_node->length = bus_node->base - hold_bus_node->base;

			hold_bus_node->next = func->bus_head;
			func->bus_head = hold_bus_node;

			temp_byte = temp_resources.bus_head->base - 1;

			// set subordinate bus
			rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_SUBORDINATE_BUS, temp_byte);

			if (temp_resources.bus_head->length == 0) {
				kfree(temp_resources.bus_head);
				temp_resources.bus_head = NULL;
			} else {
				return_resource(&(resources->bus_head), temp_resources.bus_head);
			}
		}

		// If we have IO space available and there is some left,
		// return the unused portion
		if (hold_IO_node && temp_resources.io_head) {
			io_node = do_pre_bridge_resource_split(&(temp_resources.io_head),
							       &hold_IO_node, 0x1000);

			// Check if we were able to split something off
			if (io_node) {
				hold_IO_node->base = io_node->base + io_node->length;

				temp_byte = (hold_IO_node->base) >> 8;
				rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_IO_BASE, temp_byte);

				return_resource(&(resources->io_head), io_node);
			}

			io_node = do_bridge_resource_split(&(temp_resources.io_head), 0x1000);

			// Check if we were able to split something off
			if (io_node) {
				// First use the temporary node to store information for the board
				hold_IO_node->length = io_node->base - hold_IO_node->base;

				// If we used any, add it to the board's list
				if (hold_IO_node->length) {
					hold_IO_node->next = func->io_head;
					func->io_head = hold_IO_node;

					temp_byte = (io_node->base - 1) >> 8;
					rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_IO_LIMIT, temp_byte);

					return_resource(&(resources->io_head), io_node);
				} else {
					// it doesn't need any IO
					temp_word = 0x0000;
					pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_IO_LIMIT, temp_word);

					return_resource(&(resources->io_head), io_node);
					kfree(hold_IO_node);
				}
			} else {
				// it used most of the range
				hold_IO_node->next = func->io_head;
				func->io_head = hold_IO_node;
			}
		} else if (hold_IO_node) {
			// it used the whole range
			hold_IO_node->next = func->io_head;
			func->io_head = hold_IO_node;
		}
		// If we have memory space available and there is some left,
		// return the unused portion
		if (hold_mem_node && temp_resources.mem_head) {
			mem_node = do_pre_bridge_resource_split(&(temp_resources.  mem_head),
								&hold_mem_node, 0x100000);

			// Check if we were able to split something off
			if (mem_node) {
				hold_mem_node->base = mem_node->base + mem_node->length;

				temp_word = (hold_mem_node->base) >> 16;
				rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_BASE, temp_word);

				return_resource(&(resources->mem_head), mem_node);
			}

			mem_node = do_bridge_resource_split(&(temp_resources.mem_head), 0x100000);

			// Check if we were able to split something off
			if (mem_node) {
				// First use the temporary node to store information for the board
				hold_mem_node->length = mem_node->base - hold_mem_node->base;

				if (hold_mem_node->length) {
					hold_mem_node->next = func->mem_head;
					func->mem_head = hold_mem_node;

					// configure end address
					temp_word = (mem_node->base - 1) >> 16;
					rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_LIMIT, temp_word);

					// Return unused resources to the pool
					return_resource(&(resources->mem_head), mem_node);
				} else {
					// it doesn't need any Mem
					temp_word = 0x0000;
					rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_MEMORY_LIMIT, temp_word);

					return_resource(&(resources->mem_head), mem_node);
					kfree(hold_mem_node);
				}
			} else {
				// it used most of the range
				hold_mem_node->next = func->mem_head;
				func->mem_head = hold_mem_node;
			}
		} else if (hold_mem_node) {
			// it used the whole range
			hold_mem_node->next = func->mem_head;
			func->mem_head = hold_mem_node;
		}
		// If we have prefetchable memory space available and there is some 
		// left at the end, return the unused portion
		if (hold_p_mem_node && temp_resources.p_mem_head) {
			p_mem_node = do_pre_bridge_resource_split(&(temp_resources.p_mem_head),
								  &hold_p_mem_node, 0x100000);

			// Check if we were able to split something off
			if (p_mem_node) {
				hold_p_mem_node->base = p_mem_node->base + p_mem_node->length;

				temp_word = (hold_p_mem_node->base) >> 16;
				rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_BASE, temp_word);

				return_resource(&(resources->p_mem_head), p_mem_node);
			}

			p_mem_node = do_bridge_resource_split(&(temp_resources.p_mem_head), 0x100000);

			// Check if we were able to split something off
			if (p_mem_node) {
				// First use the temporary node to store information for the board
				hold_p_mem_node->length = p_mem_node->base - hold_p_mem_node->base;

				// If we used any, add it to the board's list
				if (hold_p_mem_node->length) {
					hold_p_mem_node->next = func->p_mem_head;
					func->p_mem_head = hold_p_mem_node;

					temp_word = (p_mem_node->base - 1) >> 16;
					rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_LIMIT, temp_word);

					return_resource(&(resources->p_mem_head), p_mem_node);
				} else {
					// it doesn't need any PMem
					temp_word = 0x0000;
					rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_PREF_MEMORY_LIMIT, temp_word);

					return_resource(&(resources->p_mem_head), p_mem_node);
					kfree(hold_p_mem_node);
				}
			} else {
				// it used the most of the range
				hold_p_mem_node->next = func->p_mem_head;
				func->p_mem_head = hold_p_mem_node;
			}
		} else if (hold_p_mem_node) {
			// it used the whole range
			hold_p_mem_node->next = func->p_mem_head;
			func->p_mem_head = hold_p_mem_node;
		}
		// We should be configuring an IRQ and the bridge's base address
		// registers if it needs them.  Although we have never seen such
		// a device

		// enable card
		command = 0x0157;	// = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |  PCI_COMMAND_INVALIDATE | PCI_COMMAND_PARITY | PCI_COMMAND_SERR
		rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_COMMAND, command);

		// set Bridge Control Register
		command = 0x07;		// = PCI_BRIDGE_CTL_PARITY | PCI_BRIDGE_CTL_SERR | PCI_BRIDGE_CTL_NO_ISA
		rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_BRIDGE_CONTROL, command);
	} else if ((temp_byte & 0x7F) == PCI_HEADER_TYPE_NORMAL) {
		// Standard device
		rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, 0x0B, &class_code);

		if (class_code == PCI_BASE_CLASS_DISPLAY) {
			// Display (video) adapter (not supported)
			return(DEVICE_TYPE_NOT_SUPPORTED);
		}
		// Figure out IO and memory needs
		for (cloop = 0x10; cloop <= 0x24; cloop += 4) {
			temp_register = 0xFFFFFFFF;

			dbg("CND: bus=%d, device=%d, func=%d, offset=%d\n", func->bus, func->device, func->function, cloop);
			rc = pci_write_config_dword_nodev(ctrl->pci_ops, func->bus, func->device, func->function, cloop, temp_register);

			rc = pci_read_config_dword_nodev (ctrl->pci_ops, func->bus, func->device, func->function, cloop, &temp_register);
			dbg("CND: base = 0x%x\n", temp_register);

			if (temp_register) {	  // If this register is implemented
				if ((temp_register & 0x03L) == 0x01) {
					// Map IO

					// set base = amount of IO space
					base = temp_register & 0xFFFFFFFC;
					base = ~base + 1;

					dbg("CND:      length = 0x%x\n", base);
					io_node = get_io_resource(&(resources->io_head), base);
					dbg("Got io_node start = %8.8x, length = %8.8x next (%p)\n",
					    io_node->base, io_node->length, io_node->next);
					dbg("func (%p) io_head (%p)\n", func, func->io_head);

					// allocate the resource to the board
					if (io_node) {
						base = io_node->base;

						io_node->next = func->io_head;
						func->io_head = io_node;
					} else
						return -ENOMEM;
				} else if ((temp_register & 0x0BL) == 0x08) {
					// Map prefetchable memory
					base = temp_register & 0xFFFFFFF0;
					base = ~base + 1;

					dbg("CND:      length = 0x%x\n", base);
					p_mem_node = get_resource(&(resources->p_mem_head), base);

					// allocate the resource to the board
					if (p_mem_node) {
						base = p_mem_node->base;

						p_mem_node->next = func->p_mem_head;
						func->p_mem_head = p_mem_node;
					} else
						return -ENOMEM;
				} else if ((temp_register & 0x0BL) == 0x00) {
					// Map memory
					base = temp_register & 0xFFFFFFF0;
					base = ~base + 1;

					dbg("CND:      length = 0x%x\n", base);
					mem_node = get_resource(&(resources->mem_head), base);

					// allocate the resource to the board
					if (mem_node) {
						base = mem_node->base;

						mem_node->next = func->mem_head;
						func->mem_head = mem_node;
					} else
						return -ENOMEM;
				} else if ((temp_register & 0x0BL) == 0x04) {
					// Map memory
					base = temp_register & 0xFFFFFFF0;
					base = ~base + 1;

					dbg("CND:      length = 0x%x\n", base);
					mem_node = get_resource(&(resources->mem_head), base);

					// allocate the resource to the board
					if (mem_node) {
						base = mem_node->base;

						mem_node->next = func->mem_head;
						func->mem_head = mem_node;
					} else
						return -ENOMEM;
				} else if ((temp_register & 0x0BL) == 0x06) {
					// Those bits are reserved, we can't handle this
					return(1);
				} else {
					// Requesting space below 1M
					return(NOT_ENOUGH_RESOURCES);
				}

				rc = pci_write_config_dword_nodev(ctrl->pci_ops, func->bus, func->device, func->function, cloop, base);

				// Check for 64-bit base
				if ((temp_register & 0x07L) == 0x04) {
					cloop += 4;

					// Upper 32 bits of address always zero on today's systems
					// FIXME this is probably not true on Alpha and ia64???
					base = 0;
					rc = pci_write_config_dword_nodev(ctrl->pci_ops, func->bus, func->device, func->function, cloop, base);
				}
			}
		}		// End of base register loop

		// Figure out which interrupt pin this function uses
		rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, PCI_INTERRUPT_PIN, &temp_byte);

		// If this function needs an interrupt and we are behind a bridge
		// and the pin is tied to something that's alread mapped,
		// set this one the same
		if (temp_byte && resources->irqs && 
		    (resources->irqs->valid_INT & 
		     (0x01 << ((temp_byte + resources->irqs->barber_pole - 1) & 0x03)))) {
			// We have to share with something already set up
			IRQ = resources->irqs->interrupt[(temp_byte + resources->irqs->barber_pole - 1) & 0x03];
		} else {
			// Program IRQ based on card type
			rc = pci_read_config_byte_nodev (ctrl->pci_ops, func->bus, func->device, func->function, 0x0B, &class_code);

			if (class_code == PCI_BASE_CLASS_STORAGE) {
				IRQ = cpqhp_disk_irq;
			} else {
				IRQ = cpqhp_nic_irq;
			}
		}

		// IRQ Line
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_INTERRUPT_LINE, IRQ);

		if (!behind_bridge) {
			rc = cpqhp_set_irq(func->bus, func->device, temp_byte + 0x09, IRQ);
			if (rc)
				return(1);
		} else {
			//TBD - this code may also belong in the other clause of this If statement
			resources->irqs->interrupt[(temp_byte + resources->irqs->barber_pole - 1) & 0x03] = IRQ;
			resources->irqs->valid_INT |= 0x01 << (temp_byte + resources->irqs->barber_pole - 1) & 0x03;
		}

		// Latency Timer
		temp_byte = 0x40;
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_LATENCY_TIMER, temp_byte);

		// Cache Line size
		temp_byte = 0x08;
		rc = pci_write_config_byte_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_CACHE_LINE_SIZE, temp_byte);

		// disable ROM base Address
		temp_dword = 0x00L;
		rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_ROM_ADDRESS, temp_dword);

		// enable card
		temp_word = 0x0157;	// = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |  PCI_COMMAND_INVALIDATE | PCI_COMMAND_PARITY | PCI_COMMAND_SERR
		rc = pci_write_config_word_nodev(ctrl->pci_ops, func->bus, func->device, func->function, PCI_COMMAND, temp_word);
	}			// End of Not-A-Bridge else
	else {
		// It's some strange type of PCI adapter (Cardbus?)
		return(DEVICE_TYPE_NOT_SUPPORTED);
	}

	func->configured = 1;

	return 0;
}

