#include <linux/config.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/notifier.h>
#include <net/ip.h>
#include <net/route.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/compiler.h>
#include <net/sock.h>
#include <net/pkt_sched.h>
#include <linux/rbtree.h>

#define container_of    list_entry

struct mytype {
    rb_node_t my_node;
    int num;
};
 
struct mytype *my_search(rb_root_t *root, int num)
{
    struct mytype *data;
    rb_node_t *node = root->rb_node;
 
    while (node) {
	data = container_of(node, struct mytype, my_node);
 
	if (num < data->num)
	    node = node->rb_left;
	else if (num > data->num)
	    node = node->rb_right;
	else
	    return data;
    }
    
    return NULL;
}
 
int my_insert(rb_root_t *root, struct mytype *data)
{
    struct mytype *this;
    rb_node_t **tmp = &(root->rb_node), *parent = NULL;
 
    /* Figure out where to put new node */
    while (*tmp) {
        this = container_of(*tmp, struct mytype, my_node);
 
	parent = *tmp;
	if (data->num < this->num)
	    tmp = &((*tmp)->rb_left);
	else if (data->num > this->num)
	    tmp = &((*tmp)->rb_right);
	else 
	    return -1;
    }
    
    /* Add new node and rebalance tree. */
    rb_link_node(&data->my_node, parent, tmp);
    rb_insert_color(&data->my_node, root);
    
    return 0;
}
 
void my_delete(rb_root_t *root, int num)
{
    struct mytype *data = my_search(root, num);
    if (!data) { 
	printk(KERN_INFO "Not found %d.\n", num);
	return;
    }
    
    rb_erase(&data->my_node, root);
    kfree(data);
}
 
#if 0
void print_rbtree(rb_root_t *tree)
{
    rb_node_t *node;
    
    for (node = rb_first(tree); node; node = rb_next(node))
	printk(KERN_INFO "%d ", rb_entry(node, struct mytype, my_node)->num);
    
    printk(KERN_INFO "\n");
}
#endif
 
static int __init rbtree_test_init (void)
{
    printk ("rbtree_test_init...\n");
    rb_root_t mytree = RB_ROOT;
    char data[10] = { 23, 4, 56, 32, 89, 122, 12, 21, 45, 23 };
    int i, ret, num = 10;
    struct mytype tmp[10];
 
    printk(KERN_INFO "Please enter %d integers:\n", num);
    for (i = 0; i < num; i++) {
        tmp[i].num = data[i];
	
	ret = my_insert(&mytree, &tmp[i]);
	if (ret < 0) {
	    printk(KERN_INFO "The %d already exists.\n", tmp[i].num);
	}
    }
 
#if 0
    printk(KERN_INFO "\nthe first test\n");
    print_rbtree(&mytree);
#endif
 
    //my_delete(&mytree, 21);
 
#if 0
    printk(KERN_INFO "\nthe second test\n");
    print_rbtree(&mytree);
#endif
 
    return 0;
}

static void __exit rbtree_test_exit(void)
{
    printk ("rbtree_test_exit...\n");
}

module_init(rbtree_test_init);
module_exit(rbtree_test_exit);

MODULE_AUTHOR ("Sndrz Teo <sndrz@outlook.com>");
MODULE_DESCRIPTION ("RBTree test driver");
MODULE_LICENSE("GPL");

