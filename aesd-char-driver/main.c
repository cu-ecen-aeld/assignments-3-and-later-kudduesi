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
#include <linux/fs.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0;
int aesd_minor =   0;

MODULE_AUTHOR("kudduesi"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = container_of(inode->i_cdev ,struct aesd_dev ,cdev);
    filp->private_data = dev;
    PDEBUG("device open called\n");
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("device release called\n");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t read = 0;

    PDEBUG("read requested: count=%zu pos=%lld\n", count, *f_pos);
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    while (read < count) {
        size_t entry_off;
        struct aesd_buffer_entry *ent =
            aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->buffer, *f_pos, &entry_off);
        size_t to_copy;

        if (!ent)
            break;

        to_copy = min(ent->size - entry_off, count - read);
        if (copy_to_user(buf + read, ent->buffptr + entry_off, to_copy)) {
            retval = -EFAULT;
            goto read_done;
        }
        read += to_copy;
        *f_pos += to_copy;
        retval = read;
    }

read_done:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    char *newline;

    PDEBUG("write requested: count=%zu\n", count);
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (dev->work.buffptr) {
        char *tmp = krealloc(dev->work.buffptr,
                             dev->work.size + count,
                             GFP_KERNEL);
        if (!tmp) {
            retval = -ENOMEM;
            goto write_done;
        }
        dev->work.buffptr = tmp;
    } else {
        dev->work.buffptr = kmalloc(count, GFP_KERNEL);
        if (!dev->work.buffptr) {
            retval = -ENOMEM;
            goto write_done;
        }
    }

    if (copy_from_user(dev->work.buffptr + dev->work.size,
                       buf, count)) {
        retval = -EFAULT;
        goto write_done;
    }
    dev->work.size += count;
    retval = count;

    newline = memchr(dev->work.buffptr, '\n', dev->work.size);
    if (newline) {
        size_t entry_len = newline - dev->work.buffptr + 1;
        struct aesd_buffer_entry new = {
            .buffptr = dev->work.buffptr,
            .size = entry_len,
        };
        const char *old = aesd_circular_buffer_add_entry(
                            &dev->buffer, &new);
        if (old)
            kfree((void *)old);

        if (dev->work.size > entry_len) {
            size_t rem = dev->work.size - entry_len;
            char *keep = kmalloc(rem, GFP_KERNEL);
            if (keep) {
                memcpy(keep, dev->work.buffptr + entry_len, rem);
                dev->work.buffptr = keep;
                dev->work.size = rem;
            } else {
                kfree(dev->work.buffptr);
                dev->work.buffptr = NULL;
                dev->work.size = 0;
            }
        } else {
            dev->work.buffptr = NULL;
            dev->work.size = 0;
        }
    }

write_done:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    loff_t total_size = 0;
    uint8_t i;
    struct aesd_buffer_entry *entry;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, i) {
        total_size += entry->size;
    }

    switch (whence) {
    case SEEK_SET:
        newpos = offset;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + offset;
        break;
    case SEEK_END:
        newpos = total_size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (newpos < 0 || newpos > total_size)
        return -EINVAL;

    filp->f_pos = newpos;
    return newpos;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    uint8_t count, idx;
    loff_t newpos = 0;
    struct aesd_buffer_entry *entry;

    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;
    if (copy_from_user(&seekto, (void __user *)arg, sizeof(seekto)))
        return -EFAULT;

    if (dev->buffer.full)
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else if (dev->buffer.in_offs >= dev->buffer.out_offs)
        count = dev->buffer.in_offs - dev->buffer.out_offs;
    else
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
              + dev->buffer.in_offs - dev->buffer.out_offs;

    if (seekto.write_cmd  >= count ||
        seekto.write_cmd_offset > dev->buffer.entry[
          (dev->buffer.out_offs + seekto.write_cmd)
          % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED].size)
        return -EINVAL;

    idx = dev->buffer.out_offs;
    for (uint8_t i = 0; i < seekto.write_cmd; i++) {
        newpos += dev->buffer.entry[idx].size;
        idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    newpos += seekto.write_cmd_offset;

    filp->f_pos = newpos;
    return 0;
}

struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .read    = aesd_read,
    .write   = aesd_write,
    .open    = aesd_open,
    .release = aesd_release,
    .unlocked_ioctl = aesd_ioctl,
    .llseek  = aesd_llseek,
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
    dev_t dev;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    if (result < 0)
        return result;
    aesd_major = MAJOR(dev);

    memset(&aesd_device, 0, sizeof(aesd_device));
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);
    if (result)
        unregister_chrdev_region(dev, 1);
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t i;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, i) {
        kfree(entry->buffptr);
    }
    kfree(aesd_device.work.buffptr);
    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
