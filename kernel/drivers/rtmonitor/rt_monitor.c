
/*
 *	Real Time monitoring 
 *
 *  Author: Simon Ortego Parra
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>

#define DRIVER_AUTHOR "Simon Ortego Parra <611244@unizar.es>"
#define DRIVER_DESC "RT Monitoring driver"

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

static int __init enable_mod(void)
{
        printk(KERN_INFO "RT monitoring enabled\n");
        return 0;
}

static void __exit disable_mod(void)
{
        printk(KERN_INFO "RT monitoring disabled\n");
}

module_init(enable_mod);
module_exit(disable_mod);
