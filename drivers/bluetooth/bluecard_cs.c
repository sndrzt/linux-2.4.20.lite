/*
 *
 *  Bluetooth driver for the Anycom BlueCard (LSE039/LSE041)
 *
 *  Copyright (C) 2001-2002  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  Software distributed under the License is distributed on an "AS
 *  IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 *  implied. See the License for the specific language governing
 *  rights and limitations under the License.
 *
 *  The initial developer of the original code is David A. Hinds
 *  <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
 *  are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <asm/io.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ciscode.h>
#include <pcmcia/ds.h>
#include <pcmcia/cisreg.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>



/* ======================== Module parameters ======================== */


/* Bit map of interrupts to choose from */
static u_int irq_mask = 0x86bc;
static int irq_list[4] = { -1 };

MODULE_PARM(irq_mask, "i");
MODULE_PARM(irq_list, "1-4i");

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("BlueZ driver for the Anycom BlueCard (LSE039/LSE041)");
MODULE_LICENSE("GPL");



/* ======================== Local structures ======================== */


typedef struct bluecard_info_t {
	dev_link_t link;
	dev_node_t node;

	struct hci_dev hdev;

	spinlock_t lock;		/* For serializing operations */
	struct timer_list timer;	/* For LED control */

	struct sk_buff_head txq;
	unsigned long tx_state;

	unsigned long rx_state;
	unsigned long rx_count;
	struct sk_buff *rx_skb;

	unsigned char ctrl_reg;
	unsigned long hw_state;		/* Status of the hardware and LED control */
} bluecard_info_t;


void bluecard_config(dev_link_t *link);
void bluecard_release(u_long arg);
int bluecard_event(event_t event, int priority, event_callback_args_t *args);

static dev_info_t dev_info = "bluecard_cs";

dev_link_t *bluecard_attach(void);
void bluecard_detach(dev_link_t *);

static dev_link_t *dev_list = NULL;


/* Default baud rate: 57600, 115200, 230400 or 460800 */
#define DEFAULT_BAUD_RATE  230400


/* Hardware states */
#define CARD_READY             1
#define CARD_HAS_PCCARD_ID     4
#define CARD_HAS_POWER_LED     5
#define CARD_HAS_ACTIVITY_LED  6

/* Transmit states  */
#define XMIT_SENDING         1
#define XMIT_WAKEUP          2
#define XMIT_BUFFER_NUMBER   5	/* unset = buffer one, set = buffer two */
#define XMIT_BUF_ONE_READY   6
#define XMIT_BUF_TWO_READY   7
#define XMIT_SENDING_READY   8

/* Receiver states */
#define RECV_WAIT_PACKET_TYPE   0
#define RECV_WAIT_EVENT_HEADER  1
#define RECV_WAIT_ACL_HEADER    2
#define RECV_WAIT_SCO_HEADER    3
#define RECV_WAIT_DATA          4

/* Special packet types */
#define PKT_BAUD_RATE_57600   0x80
#define PKT_BAUD_RATE_115200  0x81
#define PKT_BAUD_RATE_230400  0x82
#define PKT_BAUD_RATE_460800  0x83


/* These are the register offsets */
#define REG_COMMAND     0x20
#define REG_INTERRUPT   0x21
#define REG_CONTROL     0x22
#define REG_RX_CONTROL  0x24
#define REG_CARD_RESET  0x30
#define REG_LED_CTRL    0x30

/* REG_COMMAND */
#define REG_COMMAND_TX_BUF_ONE  0x01
#define REG_COMMAND_TX_BUF_TWO  0x02
#define REG_COMMAND_RX_BUF_ONE  0x04
#define REG_COMMAND_RX_BUF_TWO  0x08
#define REG_COMMAND_RX_WIN_ONE  0x00
#define REG_COMMAND_RX_WIN_TWO  0x10

/* REG_CONTROL */
#define REG_CONTROL_BAUD_RATE_57600   0x00
#define REG_CONTROL_BAUD_RATE_115200  0x01
#define REG_CONTROL_BAUD_RATE_230400  0x02
#define REG_CONTROL_BAUD_RATE_460800  0x03
#define REG_CONTROL_RTS               0x04
#define REG_CONTROL_BT_ON             0x08
#define REG_CONTROL_BT_RESET          0x10
#define REG_CONTROL_BT_RES_PU         0x20
#define REG_CONTROL_INTERRUPT         0x40
#define REG_CONTROL_CARD_RESET        0x80

/* REG_RX_CONTROL */
#define RTS_LEVEL_SHIFT_BITS  0x02



/* ======================== LED handling routines ======================== */


void bluecard_activity_led_timeout(u_long arg)
{
	bluecard_info_t *info = (bluecard_info_t *)arg;
	unsigned int iobase = info->link.io.BasePort1;

	if (test_bit(CARD_HAS_ACTIVITY_LED, &(info->hw_state))) {
		/* Disable activity LED */
		outb(0x08 | 0x20, iobase + 0x30);
	} else {
		/* Disable power LED */
		outb(0x00, iobase + 0x30);
	}
}


static void bluecard_enable_activity_led(bluecard_info_t *info)
{
	unsigned int iobase = info->link.io.BasePort1;

	if (test_bit(CARD_HAS_ACTIVITY_LED, &(info->hw_state))) {
		/* Enable activity LED */
		outb(0x10 | 0x40, iobase + 0x30);

		/* Stop the LED after HZ/4 */
		mod_timer(&(info->timer), jiffies + HZ / 4);
	} else {
		/* Enable power LED */
		outb(0x08 | 0x20, iobase + 0x30);

		/* Stop the LED after HZ/2 */
		mod_timer(&(info->timer), jiffies + HZ / 2);
	}
}



/* ======================== Interrupt handling ======================== */


static int bluecard_write(unsigned int iobase, unsigned int offset, __u8 *buf, int len)
{
	int i, actual;

	actual = (len > 15) ? 15 : len;

	outb_p(actual, iobase + offset);

	for (i = 0; i < actual; i++)
		outb_p(buf[i], iobase + offset + i + 1);

	return actual;
}


static void bluecard_write_wakeup(bluecard_info_t *info)
{
	if (!info) {
		printk(KERN_WARNING "bluecard_cs: Call of write_wakeup for unknown device.\n");
		return;
	}

	if (!test_bit(XMIT_SENDING_READY, &(info->tx_state)))
		return;

	if (test_and_set_bit(XMIT_SENDING, &(info->tx_state))) {
		set_bit(XMIT_WAKEUP, &(info->tx_state));
		return;
	}

	do {
		register unsigned int iobase = info->link.io.BasePort1;
		register unsigned int offset;
		register unsigned char command;
		register unsigned long ready_bit;
		register struct sk_buff *skb;
		register int len;

		clear_bit(XMIT_WAKEUP, &(info->tx_state));

		if (!(info->link.state & DEV_PRESENT))
			return;

		if (test_bit(XMIT_BUFFER_NUMBER, &(info->tx_state))) {
			if (!test_bit(XMIT_BUF_TWO_READY, &(info->tx_state)))
				break;
			offset = 0x10;
			command = REG_COMMAND_TX_BUF_TWO;
			ready_bit = XMIT_BUF_TWO_READY;
		} else {
			if (!test_bit(XMIT_BUF_ONE_READY, &(info->tx_state)))
				break;
			offset = 0x00;
			command = REG_COMMAND_TX_BUF_ONE;
			ready_bit = XMIT_BUF_ONE_READY;
		}

		if (!(skb = skb_dequeue(&(info->txq))))
			break;

		if (skb->pkt_type & 0x80) {
			/* Disable RTS */
			info->ctrl_reg |= REG_CONTROL_RTS;
			outb(info->ctrl_reg, iobase + REG_CONTROL);
		}

		/* Activate LED */
		bluecard_enable_activity_led(info);

		/* Send frame */
		len = bluecard_write(iobase, offset, skb->data, skb->len);

		/* Tell the FPGA to send the data */
		outb_p(command, iobase + REG_COMMAND);

		/* Mark the buffer as dirty */
		clear_bit(ready_bit, &(info->tx_state));

		if (skb->pkt_type & 0x80) {

			struct wait_queue_head_t wait;
			unsigned char baud_reg;

			switch (skb->pkt_type) {
			case PKT_BAUD_RATE_460800:
				baud_reg = REG_CONTROL_BAUD_RATE_460800;
				break;
			case PKT_BAUD_RATE_230400:
				baud_reg = REG_CONTROL_BAUD_RATE_230400;
				break;
			case PKT_BAUD_RATE_115200:
				baud_reg = REG_CONTROL_BAUD_RATE_115200;
				break;
			case PKT_BAUD_RATE_57600:
				/* Fall through... */
			default:
				baud_reg = REG_CONTROL_BAUD_RATE_57600;
				break;
			}

			/* Wait until the command reaches the baseband */
			init_waitqueue_head(&wait);
			interruptible_sleep_on_timeout(&wait, HZ / 10);

			/* Set baud on baseband */
			info->ctrl_reg &= ~0x03;
			info->ctrl_reg |= baud_reg;
			outb(info->ctrl_reg, iobase + REG_CONTROL);

			/* Enable RTS */
			info->ctrl_reg &= ~REG_CONTROL_RTS;
			outb(info->ctrl_reg, iobase + REG_CONTROL);

			/* Wait before the next HCI packet can be send */
			interruptible_sleep_on_timeout(&wait, HZ);

		}

		if (len == skb->len) {
			kfree_skb(skb);
		} else {
			skb_pull(skb, len);
			skb_queue_head(&(info->txq), skb);
		}

		info->hdev.stat.byte_tx += len;

		/* Change buffer */
		change_bit(XMIT_BUFFER_NUMBER, &(info->tx_state));

	} while (test_bit(XMIT_WAKEUP, &(info->tx_state)));

	clear_bit(XMIT_SENDING, &(info->tx_state));
}


static int bluecard_read(unsigned int iobase, unsigned int offset, __u8 *buf, int size)
{
	int i, n, len;

	outb(REG_COMMAND_RX_WIN_ONE, iobase + REG_COMMAND);

	len = inb(iobase + offset);
	n = 0;
	i = 1;

	while (n < len) {

		if (i == 16) {
			outb(REG_COMMAND_RX_WIN_TWO, iobase + REG_COMMAND);
			i = 0;
		}

		buf[n] = inb(iobase + offset + i);

		n++;
		i++;

	}

	return len;
}


static void bluecard_receive(bluecard_info_t *info, unsigned int offset)
{
	unsigned int iobase;
	unsigned char buf[31];
	int i, len;

	if (!info) {
		printk(KERN_WARNING "bluecard_cs: Call of receive for unknown device.\n");
		return;
	}

	iobase = info->link.io.BasePort1;

	if (test_bit(XMIT_SENDING_READY, &(info->tx_state)))
		bluecard_enable_activity_led(info);

	len = bluecard_read(iobase, offset, buf, sizeof(buf));

	for (i = 0; i < len; i++) {

		/* Allocate packet */
		if (info->rx_skb == NULL) {
			info->rx_state = RECV_WAIT_PACKET_TYPE;
			info->rx_count = 0;
			if (!(info->rx_skb = bluez_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC))) {
				printk(KERN_WARNING "bluecard_cs: Can't allocate mem for new packet.\n");
				return;
			}
		}

		if (info->rx_state == RECV_WAIT_PACKET_TYPE) {

			info->rx_skb->dev = (void *)&(info->hdev);
			info->rx_skb->pkt_type = buf[i];

			switch (info->rx_skb->pkt_type) {

			case 0x00:
				/* init packet */
				if (offset != 0x00) {
					set_bit(XMIT_BUF_ONE_READY, &(info->tx_state));
					set_bit(XMIT_BUF_TWO_READY, &(info->tx_state));
					set_bit(XMIT_SENDING_READY, &(info->tx_state));
					bluecard_write_wakeup(info);
				}

				kfree_skb(info->rx_skb);
				info->rx_skb = NULL;
				break;

			case HCI_EVENT_PKT:
				info->rx_state = RECV_WAIT_EVENT_HEADER;
				info->rx_count = HCI_EVENT_HDR_SIZE;
				break;

			case HCI_ACLDATA_PKT:
				info->rx_state = RECV_WAIT_ACL_HEADER;
				info->rx_count = HCI_ACL_HDR_SIZE;
				break;

			case HCI_SCODATA_PKT:
				info->rx_state = RECV_WAIT_SCO_HEADER;
				info->rx_count = HCI_SCO_HDR_SIZE;
				break;

			default:
				/* unknown packet */
				printk(KERN_WARNING "bluecard_cs: Unknown HCI packet with type 0x%02x received.\n", info->rx_skb->pkt_type);
				info->hdev.stat.err_rx++;

				kfree_skb(info->rx_skb);
				info->rx_skb = NULL;
				break;

			}

		} else {

			*skb_put(info->rx_skb, 1) = buf[i];
			info->rx_count--;

			if (info->rx_count == 0) {

				int dlen;
				hci_event_hdr *eh;
				hci_acl_hdr *ah;
				hci_sco_hdr *sh;

				switch (info->rx_state) {

				case RECV_WAIT_EVENT_HEADER:
					eh = (hci_event_hdr *)(info->rx_skb->data);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = eh->plen;
					break;

				case RECV_WAIT_ACL_HEADER:
					ah = (hci_acl_hdr *)(info->rx_skb->data);
					dlen = __le16_to_cpu(ah->dlen);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = dlen;
					break;

				case RECV_WAIT_SCO_HEADER:
					sh = (hci_sco_hdr *)(info->rx_skb->data);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = sh->dlen;
					break;

				case RECV_WAIT_DATA:
					hci_recv_frame(info->rx_skb);
					info->rx_skb = NULL;
					break;

				}

			}

		}


	}

	info->hdev.stat.byte_rx += len;
}


void bluecard_interrupt(int irq, void *dev_inst, struct pt_regs *regs)
{
	bluecard_info_t *info = dev_inst;
	unsigned int iobase;
	unsigned char reg;

	if (!info) {
		printk(KERN_WARNING "bluecard_cs: Call of irq %d for unknown device.\n", irq);
		return;
	}

	if (!test_bit(CARD_READY, &(info->hw_state)))
		return;

	iobase = info->link.io.BasePort1;

	spin_lock(&(info->lock));

	/* Disable interrupt */
	info->ctrl_reg &= ~REG_CONTROL_INTERRUPT;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	reg = inb(iobase + REG_INTERRUPT);

	if ((reg != 0x00) && (reg != 0xff)) {

		if (reg & 0x04) {
			bluecard_receive(info, 0x00);
			outb(0x04, iobase + REG_INTERRUPT);
			outb(REG_COMMAND_RX_BUF_ONE, iobase + REG_COMMAND);
		}

		if (reg & 0x08) {
			bluecard_receive(info, 0x10);
			outb(0x08, iobase + REG_INTERRUPT);
			outb(REG_COMMAND_RX_BUF_TWO, iobase + REG_COMMAND);
		}

		if (reg & 0x01) {
			set_bit(XMIT_BUF_ONE_READY, &(info->tx_state));
			outb(0x01, iobase + REG_INTERRUPT);
			bluecard_write_wakeup(info);
		}

		if (reg & 0x02) {
			set_bit(XMIT_BUF_TWO_READY, &(info->tx_state));
			outb(0x02, iobase + REG_INTERRUPT);
			bluecard_write_wakeup(info);
		}

	}

	/* Enable interrupt */
	info->ctrl_reg |= REG_CONTROL_INTERRUPT;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	spin_unlock(&(info->lock));
}



/* ======================== Device specific HCI commands ======================== */


static int bluecard_hci_set_baud_rate(struct hci_dev *hdev, int baud)
{
	bluecard_info_t *info = (bluecard_info_t *)(hdev->driver_data);
	struct sk_buff *skb;

	/* Ericsson baud rate command */
	unsigned char cmd[] = { HCI_COMMAND_PKT, 0x09, 0xfc, 0x01, 0x03 };

	if (!(skb = bluez_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC))) {
		printk(KERN_WARNING "bluecard_cs: Can't allocate mem for new packet.\n");
		return -1;
	}

	switch (baud) {
	case 460800:
		cmd[4] = 0x00;
		skb->pkt_type = PKT_BAUD_RATE_460800;
		break;
	case 230400:
		cmd[4] = 0x01;
		skb->pkt_type = PKT_BAUD_RATE_230400;
		break;
	case 115200:
		cmd[4] = 0x02;
		skb->pkt_type = PKT_BAUD_RATE_115200;
		break;
	case 57600:
		/* Fall through... */
	default:
		cmd[4] = 0x03;
		skb->pkt_type = PKT_BAUD_RATE_57600;
		break;
	}

	memcpy(skb_put(skb, sizeof(cmd)), cmd, sizeof(cmd));

	skb_queue_tail(&(info->txq), skb);

	bluecard_write_wakeup(info);

	return 0;
}



/* ======================== HCI interface ======================== */


static int bluecard_hci_flush(struct hci_dev *hdev)
{
	bluecard_info_t *info = (bluecard_info_t *)(hdev->driver_data);

	/* Drop TX queue */
	skb_queue_purge(&(info->txq));

	return 0;
}


static int bluecard_hci_open(struct hci_dev *hdev)
{
	bluecard_info_t *info = (bluecard_info_t *)(hdev->driver_data);
	unsigned int iobase = info->link.io.BasePort1;

	bluecard_hci_set_baud_rate(hdev, DEFAULT_BAUD_RATE);

	if (test_and_set_bit(HCI_RUNNING, &(hdev->flags)))
		return 0;

	/* Enable LED */
	outb(0x08 | 0x20, iobase + 0x30);

	return 0;
}


static int bluecard_hci_close(struct hci_dev *hdev)
{
	bluecard_info_t *info = (bluecard_info_t *)(hdev->driver_data);
	unsigned int iobase = info->link.io.BasePort1;

	if (!test_and_clear_bit(HCI_RUNNING, &(hdev->flags)))
		return 0;

	bluecard_hci_flush(hdev);

	/* Disable LED */
	outb(0x00, iobase + 0x30);

	return 0;
}


static int bluecard_hci_send_frame(struct sk_buff *skb)
{
	bluecard_info_t *info;
	struct hci_dev *hdev = (struct hci_dev *)(skb->dev);

	if (!hdev) {
		printk(KERN_WARNING "bluecard_cs: Frame for unknown HCI device (hdev=NULL).");
		return -ENODEV;
	}

	info = (bluecard_info_t *)(hdev->driver_data);

	switch (skb->pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	};

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &(skb->pkt_type), 1);
	skb_queue_tail(&(info->txq), skb);

	bluecard_write_wakeup(info);

	return 0;
}


static void bluecard_hci_destruct(struct hci_dev *hdev)
{
}


static int bluecard_hci_ioctl(struct hci_dev *hdev, unsigned int cmd, unsigned long arg)
{
	return -ENOIOCTLCMD;
}



/* ======================== Card services HCI interaction ======================== */


int bluecard_open(bluecard_info_t *info)
{
	unsigned int iobase = info->link.io.BasePort1;
	struct hci_dev *hdev;
	unsigned char id;

	spin_lock_init(&(info->lock));

	init_timer(&(info->timer));
	info->timer.function = &bluecard_activity_led_timeout;
	info->timer.data = (u_long)info;

	skb_queue_head_init(&(info->txq));

	info->rx_state = RECV_WAIT_PACKET_TYPE;
	info->rx_count = 0;
	info->rx_skb = NULL;

	id = inb(iobase + 0x30);

	if ((id & 0x0f) == 0x02)
		set_bit(CARD_HAS_PCCARD_ID, &(info->hw_state));

	if (id & 0x10)
		set_bit(CARD_HAS_POWER_LED, &(info->hw_state));

	if (id & 0x20)
		set_bit(CARD_HAS_ACTIVITY_LED, &(info->hw_state));

	/* Reset card */
	info->ctrl_reg = REG_CONTROL_BT_RESET | REG_CONTROL_CARD_RESET;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	/* Turn FPGA off */
	outb(0x80, iobase + 0x30);

	/* Wait some time */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ / 100);

	/* Turn FPGA on */
	outb(0x00, iobase + 0x30);

	/* Activate card */
	info->ctrl_reg = REG_CONTROL_BT_ON | REG_CONTROL_BT_RES_PU;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	/* Enable interrupt */
	outb(0xff, iobase + REG_INTERRUPT);
	info->ctrl_reg |= REG_CONTROL_INTERRUPT;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	/* Start the RX buffers */
	outb(REG_COMMAND_RX_BUF_ONE, iobase + REG_COMMAND);
	outb(REG_COMMAND_RX_BUF_TWO, iobase + REG_COMMAND);

	/* Signal that the hardware is ready */
	set_bit(CARD_READY, &(info->hw_state));

	/* Drop TX queue */
	skb_queue_purge(&(info->txq));

	/* Control the point at which RTS is enabled */
	outb((0x0f << RTS_LEVEL_SHIFT_BITS) | 1, iobase + REG_RX_CONTROL);

	/* Timeout before it is safe to send the first HCI packet */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((HZ * 5) / 4);		// or set it to 3/2


	/* Initialize and register HCI device */

	hdev = &(info->hdev);

	hdev->type = HCI_PCCARD;
	hdev->driver_data = info;

	hdev->open = bluecard_hci_open;
	hdev->close = bluecard_hci_close;
	hdev->flush = bluecard_hci_flush;
	hdev->send = bluecard_hci_send_frame;
	hdev->destruct = bluecard_hci_destruct;
	hdev->ioctl = bluecard_hci_ioctl;

	if (hci_register_dev(hdev) < 0) {
		printk(KERN_WARNING "bluecard_cs: Can't register HCI device %s.\n", hdev->name);
		return -ENODEV;
	}

	return 0;
}


int bluecard_close(bluecard_info_t *info)
{
	unsigned int iobase = info->link.io.BasePort1;
	struct hci_dev *hdev = &(info->hdev);

	bluecard_hci_close(hdev);

	clear_bit(CARD_READY, &(info->hw_state));

	/* Reset card */
	info->ctrl_reg = REG_CONTROL_BT_RESET | REG_CONTROL_CARD_RESET;
	outb(info->ctrl_reg, iobase + REG_CONTROL);

	/* Turn FPGA off */
	outb(0x80, iobase + 0x30);

	if (hci_unregister_dev(hdev) < 0)
		printk(KERN_WARNING "bluecard_cs: Can't unregister HCI device %s.\n", hdev->name);

	return 0;
}



/* ======================== Card services ======================== */


static void cs_error(client_handle_t handle, int func, int ret)
{
	error_info_t err = { func, ret };

	CardServices(ReportError, handle, &err);
}


dev_link_t *bluecard_attach(void)
{
	bluecard_info_t *info;
	client_reg_t client_reg;
	dev_link_t *link;
	int i, ret;

	/* Create new info device */
	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;
	memset(info, 0, sizeof(*info));

	link = &info->link;
	link->priv = info;

	link->release.function = &bluecard_release;
	link->release.data = (u_long)link;
	link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
	link->io.NumPorts1 = 8;
	link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
	link->irq.IRQInfo1 = IRQ_INFO2_VALID | IRQ_LEVEL_ID;

	if (irq_list[0] == -1)
		link->irq.IRQInfo2 = irq_mask;
	else
		for (i = 0; i < 4; i++)
			link->irq.IRQInfo2 |= 1 << irq_list[i];

	link->irq.Handler = bluecard_interrupt;
	link->irq.Instance = info;

	link->conf.Attributes = CONF_ENABLE_IRQ;
	link->conf.Vcc = 50;
	link->conf.IntType = INT_MEMORY_AND_IO;

	/* Register with Card Services */
	link->next = dev_list;
	dev_list = link;
	client_reg.dev_info = &dev_info;
	client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
	client_reg.EventMask =
		CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
		CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
		CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
	client_reg.event_handler = &bluecard_event;
	client_reg.Version = 0x0210;
	client_reg.event_callback_args.client_data = link;

	ret = CardServices(RegisterClient, &link->handle, &client_reg);
	if (ret != CS_SUCCESS) {
		cs_error(link->handle, RegisterClient, ret);
		bluecard_detach(link);
		return NULL;
	}

	return link;
}


void bluecard_detach(dev_link_t *link)
{
	bluecard_info_t *info = link->priv;
	dev_link_t **linkp;
	int ret;

	/* Locate device structure */
	for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
		if (*linkp == link)
			break;

	if (*linkp == NULL)
		return;

	del_timer(&link->release);
	if (link->state & DEV_CONFIG)
		bluecard_release((u_long)link);

	if (link->handle) {
		ret = CardServices(DeregisterClient, link->handle);
		if (ret != CS_SUCCESS)
			cs_error(link->handle, DeregisterClient, ret);
	}

	/* Unlink device structure, free bits */
	*linkp = link->next;

	kfree(info);
}


static int get_tuple(int fn, client_handle_t handle, tuple_t *tuple, cisparse_t *parse)
{
	int i;

	i = CardServices(fn, handle, tuple);
	if (i != CS_SUCCESS)
		return CS_NO_MORE_ITEMS;

	i = CardServices(GetTupleData, handle, tuple);
	if (i != CS_SUCCESS)
		return i;

	return CardServices(ParseTuple, handle, tuple, parse);
}


#define first_tuple(a, b, c) get_tuple(GetFirstTuple, a, b, c)
#define next_tuple(a, b, c) get_tuple(GetNextTuple, a, b, c)

void bluecard_config(dev_link_t *link)
{
	client_handle_t handle = link->handle;
	bluecard_info_t *info = link->priv;
	tuple_t tuple;
	u_short buf[256];
	cisparse_t parse;
	config_info_t config;
	int i, n, last_ret, last_fn;

	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleOffset = 0;
	tuple.TupleDataMax = 255;
	tuple.Attributes = 0;

	/* Get configuration register information */
	tuple.DesiredTuple = CISTPL_CONFIG;
	last_ret = first_tuple(handle, &tuple, &parse);
	if (last_ret != CS_SUCCESS) {
		last_fn = ParseTuple;
		goto cs_failed;
	}
	link->conf.ConfigBase = parse.config.base;
	link->conf.Present = parse.config.rmask[0];

	/* Configure card */
	link->state |= DEV_CONFIG;
	i = CardServices(GetConfigurationInfo, handle, &config);
	link->conf.Vcc = config.Vcc;

	link->conf.ConfigIndex = 0x20;
	link->io.NumPorts1 = 64;
	link->io.IOAddrLines = 6;

	for (n = 0; n < 0x400; n += 0x40) {
		link->io.BasePort1 = n ^ 0x300;
		i = CardServices(RequestIO, link->handle, &link->io);
		if (i == CS_SUCCESS)
			break;
	}

	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestIO, i);
		goto failed;
	}

	i = CardServices(RequestIRQ, link->handle, &link->irq);
	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestIRQ, i);
		link->irq.AssignedIRQ = 0;
	}

	i = CardServices(RequestConfiguration, link->handle, &link->conf);
	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestConfiguration, i);
		goto failed;
	}

	MOD_INC_USE_COUNT;

	if (bluecard_open(info) != 0)
		goto failed;

	strcpy(info->node.dev_name, info->hdev.name);
	link->dev = &info->node;
	link->state &= ~DEV_CONFIG_PENDING;

	return;

cs_failed:
	cs_error(link->handle, last_fn, last_ret);

failed:
	bluecard_release((u_long)link);
}


void bluecard_release(u_long arg)
{
	dev_link_t *link = (dev_link_t *)arg;
	bluecard_info_t *info = link->priv;

	if (link->state & DEV_PRESENT)
		bluecard_close(info);

	MOD_DEC_USE_COUNT;

	link->dev = NULL;

	CardServices(ReleaseConfiguration, link->handle);
	CardServices(ReleaseIO, link->handle, &link->io);
	CardServices(ReleaseIRQ, link->handle, &link->irq);

	link->state &= ~DEV_CONFIG;
}


int bluecard_event(event_t event, int priority, event_callback_args_t *args)
{
	dev_link_t *link = args->client_data;
	bluecard_info_t *info = link->priv;

	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG) {
			bluecard_close(info);
			mod_timer(&link->release, jiffies + HZ / 20);
		}
		break;
	case CS_EVENT_CARD_INSERTION:
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		bluecard_config(link);
		break;
	case CS_EVENT_PM_SUSPEND:
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		if (link->state & DEV_CONFIG)
			CardServices(ReleaseConfiguration, link->handle);
		break;
	case CS_EVENT_PM_RESUME:
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		if (DEV_OK(link))
			CardServices(RequestConfiguration, link->handle, &link->conf);
		break;
	}

	return 0;
}



/* ======================== Module initialization ======================== */


int __init init_bluecard_cs(void)
{
	servinfo_t serv;
	int err;

	CardServices(GetCardServicesInfo, &serv);
	if (serv.Revision != CS_RELEASE_CODE) {
		printk(KERN_NOTICE "bluecard_cs: Card Services release does not match!\n");
		return -1;
	}

	err = register_pccard_driver(&dev_info, &bluecard_attach, &bluecard_detach);

	return err;
}


void __exit exit_bluecard_cs(void)
{
	unregister_pccard_driver(&dev_info);

	while (dev_list != NULL)
		bluecard_detach(dev_list);
}


module_init(init_bluecard_cs);
module_exit(exit_bluecard_cs);

EXPORT_NO_SYMBOLS;
