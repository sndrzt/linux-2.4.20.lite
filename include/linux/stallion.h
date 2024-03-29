/*****************************************************************************/

/*
 *	stallion.h  -- stallion multiport serial driver.
 *
 *	Copyright (C) 1996-1998  Stallion Technologies (support@stallion.oz.au).
 *	Copyright (C) 1994-1996  Greg Ungerer.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*****************************************************************************/
#ifndef	_STALLION_H
#define	_STALLION_H
/*****************************************************************************/

/*
 *	Define important driver constants here.
 */
#define	STL_MAXBRDS		4
#define	STL_MAXPANELS		4
#define	STL_MAXBANKS		8
#define	STL_PORTSPERPANEL	16
#define	STL_MAXPORTS		64
#define	STL_MAXDEVS		(STL_MAXBRDS * STL_MAXPORTS)


/*
 *	Define a set of structures to hold all the board/panel/port info
 *	for our ports. These will be dynamically allocated as required.
 */

/*
 *	Define a ring queue structure for each port. This will hold the
 *	TX data waiting to be output. Characters are fed into this buffer
 *	from the line discipline (or even direct from user space!) and
 *	then fed into the UARTs during interrupts. Will use a classic ring
 *	queue here for this. The good thing about this type of ring queue
 *	is that the head and tail pointers can be updated without interrupt
 *	protection - since "write" code only needs to change the head, and
 *	interrupt code only needs to change the tail.
 */
typedef struct {
	char	*buf;
	char	*head;
	char	*tail;
} stlrq_t;

/*
 *	Port, panel and board structures to hold status info about each.
 *	The board structure contains pointers to structures for each panel
 *	connected to it, and in turn each panel structure contains pointers
 *	for each port structure for each port on that panel. Note that
 *	the port structure also contains the board and panel number that it
 *	is associated with, this makes it (fairly) easy to get back to the
 *	board/panel info for a port.
 */
typedef struct stlport {
	unsigned long		magic;
	int			portnr;
	int			panelnr;
	int			brdnr;
	int			ioaddr;
	int			uartaddr;
	int			pagenr;
	long			istate;
	int			flags;
	int			baud_base;
	int			custom_divisor;
	int			close_delay;
	int			closing_wait;
	int			refcount;
	int			openwaitcnt;
	int			brklen;
	long			session;
	long			pgrp;
	unsigned int		sigs;
	unsigned int		rxignoremsk;
	unsigned int		rxmarkmsk;
	unsigned int		imr;
	unsigned int		crenable;
	unsigned long		clk;
	unsigned long		hwid;
	void			*uartp;
	struct tty_struct	*tty;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,0))
	struct wait_queue	*open_wait;
	struct wait_queue	*close_wait;
#else
	struct wait_queue_head_t	open_wait;
	struct wait_queue_head_t	close_wait;
#endif
	struct termios		normaltermios;
	struct termios		callouttermios;
	struct tq_struct	tqueue;
	comstats_t		stats;
	stlrq_t			tx;
} stlport_t;

typedef struct stlpanel {
	unsigned long	magic;
	int		panelnr;
	int		brdnr;
	int		pagenr;
	int		nrports;
	int		iobase;
	void		*uartp;
	void		(*isr)(struct stlpanel *panelp, unsigned int iobase);
	unsigned int	hwid;
	unsigned int	ackmask;
	stlport_t	*ports[STL_PORTSPERPANEL];
} stlpanel_t;

typedef struct stlbrd {
	unsigned long	magic;
	int		brdnr;
	int		brdtype;
	int		state;
	int		nrpanels;
	int		nrports;
	int		nrbnks;
	int		irq;
	int		irqtype;
	void		(*isr)(struct stlbrd *brdp);
	unsigned int	ioaddr1;
	unsigned int	ioaddr2;
	unsigned int	iosize1;
	unsigned int	iosize2;
	unsigned int	iostatus;
	unsigned int	ioctrl;
	unsigned int	ioctrlval;
	unsigned int	hwid;
	unsigned long	clk;
	unsigned int	bnkpageaddr[STL_MAXBANKS];
	unsigned int	bnkstataddr[STL_MAXBANKS];
	stlpanel_t	*bnk2panel[STL_MAXBANKS];
	stlpanel_t	*panels[STL_MAXPANELS];
} stlbrd_t;


/*
 *	Define MAGIC numbers used for above structures.
 */
#define	STL_PORTMAGIC	0x5a7182c9
#define	STL_PANELMAGIC	0x7ef621a1
#define	STL_BOARDMAGIC	0xa2267f52

/*****************************************************************************/
#endif
