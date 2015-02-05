
/*
 *	Real Time monitoring 
 *
 *  Author: Simon Ortego Parra
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rt_monitor.h>

MODULE_AUTHOR("Simon Ortego Parra <611244@unizar.es>");
MODULE_DESCRIPTION("RT Monitoring driver");
MODULE_LICENSE("GPL");


static int enabled = 0;

int rt_monitor_enabled (void)
{
	return enabled;
}
EXPORT_SYMBOL_GPL(rt_monitor_enabled); 


ssize_t rt_monitor_write(struct file * file, const char __user * buffer, size_t length, loff_t * offset)
{
	return 0;
}

/* Module specific */

static int __init rt_monitor_start(void)
{
	/*
	int ret;
	//struct rt_monitor_dev device;

	ret = register_chrdev(MAJOR_NUM, DEVICE_NAME, &rt_monitor_fops);

	if(ret < 0) {
		printk(KERN_ALERT "%s failed with %d\n",
		       "Sorry, registering the character device ", ret);
		return ret;
	}

	printk(KERN_INFO "%s The major device number is %d.\n",
	       "Registeration is a success", MAJOR_NUM);

	*/	
	enabled = 1;
	return 0;
}

static void __exit rt_monitor_stop(void)
{
	//unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
	enabled = 0;
}

module_init(rt_monitor_start);
module_exit(rt_monitor_stop);
