#include <linux/module.h>
#include <linux/kernel.h>

static int __init enable_mod(void)
{
        printk(KERN_INFO "My Kernel Module is enabled.\n");
        return 0;
}

static void __exit disable_mod(void)
{
        printk(KERN_INFO "My Kernel Module is disabled.\n");
}

module_init(enable_mod);
module_exit(disable_mod);
