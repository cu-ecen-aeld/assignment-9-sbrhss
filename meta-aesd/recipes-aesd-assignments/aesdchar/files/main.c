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
#include <linux/uaccess.h> // copy_from_user, copy_to_user
#include <linux/mutex.h> // mutex
#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Saber Hosseini"); /** My Name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset_byte = 0;
    size_t bytes_to_read;
    
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    
    if (dev == NULL || buf == NULL) {
        return -EFAULT;
    }
    
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    
    // Find the entry corresponding to the file position
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset_byte);
    
    if (entry == NULL || entry->buffptr == NULL) {
        mutex_unlock(&dev->lock);
        return 0; // EOF
    }
    
    // Calculate how many bytes we can read from this entry
    bytes_to_read = entry->size - entry_offset_byte;
    if (bytes_to_read > count) {
        bytes_to_read = count;
    }
    
    // Copy data to user space
    if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_read)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }
    
    *f_pos += bytes_to_read;
    retval = bytes_to_read;
    
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    char *write_buffer = NULL;
    char *newline_pos = NULL;
    size_t total_size;
    struct aesd_buffer_entry entry;
    struct aesd_buffer_entry *old_entry;
    
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    
    if (dev == NULL || buf == NULL) {
        return -EFAULT;
    }
    
    if (count == 0) {
        return 0;
    }
    
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    
    // Allocate buffer for the write data
    write_buffer = kmalloc(count, GFP_KERNEL);
    if (write_buffer == NULL) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }
    
    // Copy data from user space
    if (copy_from_user(write_buffer, buf, count)) {
        kfree(write_buffer);
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }
    
    // Check for newline character
    newline_pos = memchr(write_buffer, '\n', count);
    
    if (newline_pos != NULL) {
        // Found newline - complete command
        size_t newline_offset = newline_pos - write_buffer;
        total_size = dev->partial_write_size + newline_offset + 1; // +1 for newline
        
        // Allocate buffer for complete command
        char *complete_buffer = kmalloc(total_size, GFP_KERNEL);
        if (complete_buffer == NULL) {
            kfree(write_buffer);
            mutex_unlock(&dev->lock);
            return -ENOMEM;
        }
        
        // Copy partial write if exists
        if (dev->partial_write_buffer != NULL && dev->partial_write_size > 0) {
            memcpy(complete_buffer, dev->partial_write_buffer, dev->partial_write_size);
        }
        
        // Copy new data including newline
        memcpy(complete_buffer + dev->partial_write_size, write_buffer, newline_offset + 1);
        
        // Free partial write buffer
        if (dev->partial_write_buffer != NULL) {
            kfree(dev->partial_write_buffer);
            dev->partial_write_buffer = NULL;
            dev->partial_write_size = 0;
        }
        
        // Prepare entry for circular buffer
        entry.buffptr = complete_buffer;
        entry.size = total_size;
        
        // Check if we need to free old entry when buffer is full
        // The entry at in_offs will be overwritten when buffer is full
        if (dev->circular_buffer.full) {
            old_entry = &dev->circular_buffer.entry[dev->circular_buffer.in_offs];
            if (old_entry->buffptr != NULL) {
                kfree((void *)old_entry->buffptr);
            }
        }
        
        // Add entry to circular buffer
        aesd_circular_buffer_add_entry(&dev->circular_buffer, &entry);
        
        retval = newline_offset + 1;
        kfree(write_buffer);
    } else {
        // No newline - append to partial write
        size_t new_partial_size = dev->partial_write_size + count;
        char *new_partial_buffer = kmalloc(new_partial_size, GFP_KERNEL);
        if (new_partial_buffer == NULL) {
            kfree(write_buffer);
            mutex_unlock(&dev->lock);
            return -ENOMEM;
        }
        
        // Copy existing partial write if exists
        if (dev->partial_write_buffer != NULL && dev->partial_write_size > 0) {
            memcpy(new_partial_buffer, dev->partial_write_buffer, dev->partial_write_size);
            kfree(dev->partial_write_buffer);
        }
        
        // Append new data
        memcpy(new_partial_buffer + dev->partial_write_size, write_buffer, count);
        
        dev->partial_write_buffer = new_partial_buffer;
        dev->partial_write_size = new_partial_size;
        
        retval = count;
        kfree(write_buffer);
    }
    
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * Calculate total size of all entries in the circular buffer
 */
static size_t aesd_circular_buffer_total_size(struct aesd_circular_buffer *buffer)
{
    size_t total_size = 0;
    uint8_t index;
    uint8_t entries_to_process;
    uint8_t i;
    struct aesd_buffer_entry *entry;
    
    if (buffer == NULL) {
        return 0;
    }
    
    // Calculate how many entries we need to process
    if (buffer->full) {
        entries_to_process = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        if (buffer->in_offs >= buffer->out_offs) {
            entries_to_process = buffer->in_offs - buffer->out_offs;
        } else {
            entries_to_process = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs + buffer->in_offs;
        }
    }
    
    // Start from out_offs and iterate through entries
    index = buffer->out_offs;
    
    // Process each entry
    for (i = 0; i < entries_to_process; i++) {
        entry = &buffer->entry[index];
        
        // Only count valid entries
        if (entry->buffptr != NULL && entry->size > 0) {
            total_size += entry->size;
        }
        
        // Move to next index
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    
    return total_size;
}

/**
 * Find entry by command number (zero-referenced)
 * Returns the entry and calculates the base offset for that command
 */
static struct aesd_buffer_entry *aesd_circular_buffer_find_entry_by_cmd(
    struct aesd_circular_buffer *buffer,
    uint32_t write_cmd,
    size_t *base_offset)
{
    uint8_t index;
    uint8_t entries_to_process;
    uint8_t i;
    struct aesd_buffer_entry *entry = NULL;
    size_t current_offset = 0;
    
    if (buffer == NULL || base_offset == NULL) {
        return NULL;
    }
    
    // Calculate how many entries we need to process
    if (buffer->full) {
        entries_to_process = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        if (buffer->in_offs >= buffer->out_offs) {
            entries_to_process = buffer->in_offs - buffer->out_offs;
        } else {
            entries_to_process = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs + buffer->in_offs;
        }
    }
    
    // Check if write_cmd is out of range
    if (write_cmd >= entries_to_process) {
        return NULL;
    }
    
    // Start from out_offs and iterate through entries
    index = buffer->out_offs;
    
    // Process each entry until we reach the desired command
    for (i = 0; i <= write_cmd; i++) {
        entry = &buffer->entry[index];
        
        // Only process valid entries
        if (entry->buffptr != NULL && entry->size > 0) {
            if (i == write_cmd) {
                // Found the command we're looking for
                *base_offset = current_offset;
                return entry;
            }
            current_offset += entry->size;
        }
        
        // Move to next index
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    
    return NULL;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos = 0;
    size_t total_size = 0;
    
    PDEBUG("llseek offset %lld whence %d", offset, whence);
    
    if (dev == NULL) {
        return -EFAULT;
    }
    
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    
    // Calculate total size for SEEK_END
    total_size = aesd_circular_buffer_total_size(&dev->circular_buffer);
    
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
    
    // Validate the new position
    if (new_pos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    // Check if position is beyond EOF
    if (new_pos > (loff_t)total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    filp->f_pos = new_pos;
    
    mutex_unlock(&dev->lock);
    return new_pos;
}

long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    struct aesd_buffer_entry *entry = NULL;
    size_t base_offset = 0;
    loff_t new_pos;
    
    PDEBUG("ioctl cmd 0x%x arg %lu", cmd, arg);
    
    if (dev == NULL) {
        return -EFAULT;
    }
    
    if (_IOC_TYPE(cmd) != AESDCHAR_IOC_MAGIC) {
        return -ENOTTY;
    }
    
    if (_IOC_NR(cmd) != 0) {
        return -ENOTTY;
    }
    
    if (cmd != AESDCHAR_IOCSEEKTO) {
        return -ENOTTY;
    }
    
    // Copy seekto structure from user space
    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
        return -EFAULT;
    }
    
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    
    // Find the entry corresponding to the command number
    entry = aesd_circular_buffer_find_entry_by_cmd(&dev->circular_buffer, 
                                                    seekto.write_cmd, 
                                                    &base_offset);
    
    if (entry == NULL) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    // Validate offset within the command
    if (seekto.write_cmd_offset >= entry->size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    // Calculate new file position
    new_pos = base_offset + seekto.write_cmd_offset;
    
    // Update file position
    filp->f_pos = new_pos;
    
    mutex_unlock(&dev->lock);
    return 0;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
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

    // Initialize circular buffer
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    
    // Initialize mutex
    mutex_init(&aesd_device.lock);
    
    // Initialize partial write buffer
    aesd_device.partial_write_buffer = NULL;
    aesd_device.partial_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // Cleanup: free all circular buffer entries
    mutex_lock(&aesd_device.lock);
    
    // Free partial write buffer if exists
    if (aesd_device.partial_write_buffer != NULL) {
        kfree(aesd_device.partial_write_buffer);
        aesd_device.partial_write_buffer = NULL;
        aesd_device.partial_write_size = 0;
    }
    
    // Free all circular buffer entries
    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr != NULL) {
            kfree((void *)entry->buffptr);
        }
    }
    
    mutex_unlock(&aesd_device.lock);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
