

#ifndef RT_MONITOR_H

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/ioctl.h>

/* Static definition of major dev number needed for ioctls */
#define MAJOR_NUM 100
#define DEVICE_NAME "rt_monitor_dev"

#define MAX_NUM_EVENTS 100

/* Device structure */
struct rt_monitor_dev {
	struct cdev cdev;
};

int rt_monitor_enabled (void);

ssize_t rt_monitor_write(struct file *, const char __user *, size_t, loff_t *);


/* 
 * This structure will hold the functions to be called
 * when a process does something to the device we
 * created. Since a pointer to this structure is kept in
 * the devices table, it can't be local to
 * init_module. NULL is for unimplemented functions. 
 */
struct file_operations rt_monitor_fops = 
{
	.owner = THIS_MODULE,
	.write = rt_monitor_write,
};

#endif /* RT_MONITOR_H */