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
#include "aesd_ioctl.h"

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

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos = 0;
    size_t total_size = 0;
    struct aesd_buffer_entry *entry;
    int index;

    // Lock to protect the buffer while we calculate size and update position
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Calculate the total size of all data in the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry->buffptr) {
            total_size += entry->size;
        }
    }

    // Determine the new position based on the whence parameter
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = filp->f_pos + offset;
            break;
        case SEEK_END:
            new_pos = total_size + offset;
            break;
        default:
            mutex_unlock(&dev->lock);
            return -EINVAL;
    }

    // Check if the new position is valid (cannot be negative)
    // Note: We allow seeking beyond total_size (creating holes) as per standard behavior,
    // though read() will just return 0 (EOF) in that case.
    if (new_pos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    // Update the file position
    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    
    return new_pos;
}

/**
 * Perform the seek based on write_cmd (entry index) and write_cmd_offset (offset within entry)
 */
long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seek_args;
    struct aesd_buffer_entry *entry;
    long retval = 0;
    uint32_t i;
    uint8_t index;
    size_t new_pos = 0;

    // 1. Check for valid command
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;

    // 2. Switch based on the command
    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
            // Copy arguments from user space
            if (copy_from_user(&seek_args, (const void __user *)arg, sizeof(seek_args))) {
                return -EFAULT;
            }

            // Lock to protect buffer state
            if (mutex_lock_interruptible(&dev->lock)) {
                return -ERESTARTSYS;
            }

            // 3. Iterate to find the target command (write_cmd)
            // We start at out_offs (the oldest entry) and walk forward 'write_cmd' times.
            // While walking, we sum up the sizes of skipped entries to calculate the byte offset.
            
            index = dev->buffer.out_offs;
            
            // Check if the requested command index is potentially valid (max 10 entries)
            if (seek_args.write_cmd >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
                 mutex_unlock(&dev->lock);
                 return -EINVAL;
            }

            // We also need to check if the buffer actually has that many entries.
            // Since we don't track "count" explicitly, we can just walk and see if we hit NULL.
            // However, a safer way is to ensure we don't walk past in_offs if not full.
            
            // Let's walk the buffer to calculate the offset of the start of the requested command.
            for (i = 0; i < seek_args.write_cmd; i++) {
                // If the entry is valid (buffptr is not NULL), add its size
                if (dev->buffer.entry[index].buffptr == NULL) {
                    mutex_unlock(&dev->lock);
                    return -EINVAL; // User asked for command #5 but we only have 3
                }
                
                new_pos += dev->buffer.entry[index].size;
                
                // Advance to next entry in circular buffer
                index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

                // If we wrapped around to in_offs and buffer isn't full, we ran out of data
                if (index == dev->buffer.in_offs && !dev->buffer.full) {
                    mutex_unlock(&dev->lock);
                    return -EINVAL;
                }
            }

            // 4. Validate the offset WITHIN the target command
            // At this point, 'index' points to the specific entry requested.
            entry = &dev->buffer.entry[index];
            
            // Ensure the entry exists (just in case)
            if (entry->buffptr == NULL) {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }

            // Check if the requested offset is larger than the command's size
            if (seek_args.write_cmd_offset >= entry->size) {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }

            // 5. Calculate Final Position and Update
            // The position is (Start of Command) + (Offset within Command)
            new_pos += seek_args.write_cmd_offset;
            
            filp->f_pos = new_pos;
            
            PDEBUG("ioctl seek: cmd %u, offset %u -> f_pos %zu", 
                   seek_args.write_cmd, seek_args.write_cmd_offset, new_pos);

            mutex_unlock(&dev->lock);
            break;

        default:
            return -ENOTTY;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
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
