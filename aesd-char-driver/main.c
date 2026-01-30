/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h> // kmalloc, kfree
#include <linux/uaccess.h> // copy_to_user, copy_from_user
#include <linux/mutex.h>
#include "aesdchar.h"
#include "aesd-circular-buffer.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("JavierFo");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp);
int aesd_release(struct inode *inode, struct file *filp);
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
int aesd_init_module(void);
void aesd_cleanup_module(void);

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    
    PDEBUG("open");
    
    // Determine which device is being opened (standard pattern for cdev)
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    // Nothing specific to release here as memory persistence is required
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset_byte = 0;
    size_t bytes_to_copy = 0;
    
    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    // Lock the device to prevent modification of the buffer during read
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // Find the entry that corresponds to the current file position
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset_byte);
    
    if (entry) {
        // Calculate how many bytes are available in this specific entry from the offset
        size_t available_bytes = entry->size - entry_offset_byte;
        
        // If the user asked for more than is available in this entry, just return what's left in this entry
        // (The user will call read again for the rest)
        bytes_to_copy = (available_bytes > count) ? count : available_bytes;
        
        if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_copy)) {
            retval = -EFAULT;
        } else {
            retval = bytes_to_copy;
            *f_pos += bytes_to_copy; // Advance file position
        }
    } else {
        // If entry is NULL, we have reached the end of the buffer
        retval = 0;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *new_buffer;
    size_t new_size;
    
    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);
    
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // 1. Calculate the new size needed (current partial buffer + new write)
    new_size = dev->working_entry.size + count;
    
    // 2. Allocate the new buffer
    new_buffer = kmalloc(new_size, GFP_KERNEL);
    if (!new_buffer) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }

    // 3. Copy existing partial data if any
    if (dev->working_entry.size > 0) {
        memcpy(new_buffer, dev->working_entry.buffptr, dev->working_entry.size);
        kfree(dev->working_entry.buffptr); // Free the old partial buffer
    }
    
    // 4. Copy the new data from user space
    if (copy_from_user(new_buffer + dev->working_entry.size, buf, count)) {
        kfree(new_buffer);
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    // 5. Update the working entry to point to the new buffer
    dev->working_entry.buffptr = new_buffer;
    dev->working_entry.size = new_size;
    retval = count;

    // 6. Check if we found a newline character at the end of the input
    // The instructions imply a command ends with \n. 
    if (dev->working_entry.buffptr[new_size - 1] == '\n') {
        
        // Check if the circular buffer is full. If so, we are about to overwrite
        // an entry. We MUST free that memory first to avoid a leak.
        if (dev->buffer.full) {
            struct aesd_buffer_entry *oldest_entry = &dev->buffer.entry[dev->buffer.in_offs];
            if (oldest_entry->buffptr) {
                kfree(oldest_entry->buffptr);
            }
        }

        // Add the working entry to the circular buffer
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->working_entry);

        // Reset the working entry for the next command
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    // Initialize the mutex and the circular buffer
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    // Free all memory stored in the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr != NULL) {
            kfree(entry->buffptr);
        }
    }

    // Free any partial write that was in progress but not completed
    if (aesd_device.working_entry.buffptr != NULL) {
        kfree(aesd_device.working_entry.buffptr);
    }
    
    // Destroy the mutex
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
