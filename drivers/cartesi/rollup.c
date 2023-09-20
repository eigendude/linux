////// SPDX-License-Identifier: GPL-2.0
/*
 * Cartesi rollup device.
 * Copyright (C) 2021 Cartesi Pte. Ltd.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <crypto/hash.h>
#include "libyield.h"
#include <uapi/linux/cartesi/rollup.h>

#define DEVICE_NAME "rollup"
#define FDT_ROLLUP_PATH "/rollup"
#define MODULE_DESC "Cartesi Machine " DEVICE_NAME " device"

#define CARTESI_ROLLUP_INITIAL_STATE (-1)

/* NOTE: keep in sync with node_paths */
#define TX_BUFFER_INDEX      0
#define RX_BUFFER_INDEX      1

static const char *node_paths[] = {
    "tx_buffer",
    "rx_buffer",
};

static atomic_t rollup_probe_once = ATOMIC_INIT(1);

struct rollup_device {
    struct platform_device *pdev;
    struct miscdevice mdev;
    struct mutex lock;
    atomic_t rollup_status;
    int next_request_type;
    size_t input_length;
    struct rollup_bytes buffers[ARRAY_SIZE(node_paths)];
};

static struct rollup_device *to_rollup_device(struct file *file)
{
    struct miscdevice *dev = file->private_data;
    return container_of(dev, struct rollup_device, mdev);
}

static long rollup_ioctl_finish(struct rollup_device *rollup, unsigned long arg)
{
    long ret;
    u64 input_length, reason;
    struct rollup_finish finish;
    struct rollup_bytes *rx = &rollup->buffers[RX_BUFFER_INDEX];
    struct rollup_bytes *tx = &rollup->buffers[TX_BUFFER_INDEX];
    struct yield_request rep;

    if ((ret = copy_from_user(&finish, (void __user*)arg, sizeof(finish))))
        return -EFAULT;

    reason = finish.accept_previous_request?
        HTIF_YIELD_REASON_RX_ACCEPTED:
        HTIF_YIELD_REASON_RX_REJECTED;

    if (mutex_lock_interruptible(&rollup->lock))
        return -ERESTARTSYS;

    memcpy(tx->data, finish.output_hash, sizeof(finish.output_hash));
    if ((ret = cartesi_yield(HTIF_YIELD_MANUAL, reason, sizeof(finish.output_hash), &rep))) {
        ret = -EIO;
        goto unlock;
    }

    if (rep.reason != CARTESI_ROLLUP_ADVANCE_STATE &&
        rep.reason != CARTESI_ROLLUP_INSPECT_STATE) {
        ret = -EOPNOTSUPP;
        goto unlock;
    }
    reason = rep.reason;

    if (rx->length < rep.data) {
        ret = -ENOBUFS;
        goto unlock;
    }
    input_length = rep.data;

    rollup->next_request_type = reason;
    rollup->input_length = input_length;
    finish.next_request_type = reason;

    mutex_unlock(&rollup->lock);

    finish.next_request_payload_length = input_length;
    if ((ret = copy_to_user((void __user*)arg, &finish, sizeof(finish))))
        return -EFAULT;

    return 0;
unlock:
    mutex_unlock(&rollup->lock);
    return ret;
}

static long rollup_ioctl_read_input(struct rollup_device *rollup, unsigned long arg)
{
    long ret = 0;
    struct rollup_bytes input;
    struct rollup_bytes *rx = &rollup->buffers[RX_BUFFER_INDEX];

    if ((ret = copy_from_user(&input, (void __user*)arg, sizeof(input))))
        return -EFAULT;

    if (input.length < rollup->input_length)
        return -ENOBUFS;

    if (mutex_lock_interruptible(&rollup->lock))
        return -ERESTARTSYS;

    if ((ret = copy_to_user(input.data, rx->data, rollup->input_length))) {
        ret = -EFAULT;
        goto unlock;
    }

    /* fall-through */
unlock:
    mutex_unlock(&rollup->lock);
    return ret;
}

static long rollup_ioctl_write_output(struct rollup_device *rollup, unsigned long arg)
{
    long ret = 0;
    struct yield_request rep;
    struct rollup_bytes output;
    struct rollup_bytes *tx = &rollup->buffers[TX_BUFFER_INDEX];

    if ((ret = copy_from_user(&output, (void __user*)arg, sizeof(output))))
        return -EFAULT;

    if (tx->length < output.length)
            return -ENOBUFS;

    if (mutex_lock_interruptible(&rollup->lock))
        return -ERESTARTSYS;

    if ((ret = cartesi_yield(HTIF_YIELD_AUTOMATIC, HTIF_YIELD_REASON_TX_OUTPUT, output.length, &rep))) {
        ret = -EIO;
        goto unlock;
    }

    if ((ret = copy_from_user(tx->data, output.data, output.length))) {
        ret = -EFAULT;
        goto unlock;
    }

    /* fall-through */
unlock:
    mutex_unlock(&rollup->lock);
    return ret;
}

/*
 * We enforce only one user at a time here with the open/close.
 */
static int rollup_open(struct inode *inode, struct file *file)
{
    struct rollup_device *rollup;
    if ((rollup = to_rollup_device(file)) == NULL)
        return -EBADF;

    if (!atomic_dec_and_test(&rollup->rollup_status)) {
        atomic_inc(&rollup->rollup_status);
        return -EBUSY;
    }
    return 0;
}

static int rollup_release(struct inode *inode, struct file *file)
{
    struct rollup_device *rollup;
    if ((rollup = to_rollup_device(file)) == NULL)
        return -EBADF;

    atomic_inc(&rollup->rollup_status);
    return 0;
}

static long rollup_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct rollup_device *rollup;

    if ((rollup = to_rollup_device(file)) == NULL)
        return -EBADF;

    switch (cmd) {
    case IOCTL_ROLLUP_FINISH:
        return rollup_ioctl_finish(rollup, arg);
    case IOCTL_ROLLUP_READ_INPUT:
        return rollup_ioctl_read_input(rollup, arg);
    case IOCTL_ROLLUP_WRITE_OUTPUT:
        return rollup_ioctl_write_output(rollup, arg);
    }
    return -ENOIOCTLCMD;
}

static const struct file_operations rollup_fileops = {
    .open           = rollup_open,
    .release        = rollup_release,
    .owner          = THIS_MODULE,
    .unlocked_ioctl = rollup_ioctl
};

static int find_memory_regions(struct rollup_device *rollup)
{
    int i, j, err = 0;
    struct device_node *node = of_find_node_by_path(FDT_ROLLUP_PATH);

    for (i = 0; !err && i < ARRAY_SIZE(node_paths); ++i) {
        u64 xs[2];
        struct rollup_bytes *bi = &rollup->buffers[i];
        struct device_node *buffer = of_find_node_by_name(node, node_paths[i]);
        err = of_property_read_u64_array(buffer, "reg", xs, 2);

        if (xs[0] == 0)
            return -ENODEV;
        if (xs[1] == 0)
            return -EIO;

        if ((bi->data = devm_ioremap(rollup->mdev.this_device, xs[0], xs[1])) == NULL)
            return -ENODEV;
        bi->length = xs[1];

        /* do buffers intersect => malformed IO */
        for (j = 0; j < i; ++j) {
            struct rollup_bytes *bj = &rollup->buffers[j];
            if ((bi->data <= bj->data && bj->data < bi->data + bi->length) ||
                (bj->data <= bi->data && bi->data < bj->data + bj->length))
                return -EIO;
        }
    }

    return err;
}

static int rollup_driver_probe(struct platform_device *pdev)
{
    struct rollup_device *rollup;
    int ret = -ENXIO;

    if (!atomic_dec_and_test(&rollup_probe_once)) {
        atomic_inc(&rollup_probe_once);
        return -EBUSY;
    }

    rollup = (struct rollup_device*) kzalloc(sizeof(struct rollup_device), GFP_KERNEL);
    if (!rollup) {
        dev_err(&pdev->dev, "failed to allocate memory\n");
        return -ENOMEM;
    }

    atomic_set(&rollup->rollup_status, 1);
    rollup->mdev.minor = MISC_DYNAMIC_MINOR;
    rollup->mdev.name  = DEVICE_NAME;
    rollup->mdev.fops  = &rollup_fileops;
    if ((ret = misc_register(&rollup->mdev)) != 0) {
        dev_err(&pdev->dev, "failed to register miscdevice\n");
        goto free_rollup;
    }

    if ((ret = find_memory_regions(rollup)) != 0) {
        dev_err(&pdev->dev, "failed to parse device tree\n");
        goto free_miscdevice;
    }

    platform_set_drvdata(pdev, rollup);
    rollup->pdev = pdev;
    rollup->next_request_type = CARTESI_ROLLUP_INITIAL_STATE;
    rollup->input_length = 0;

    mutex_init(&rollup->lock);
    pr_info(MODULE_DESC ": Module loaded\n");
    return 0;

free_miscdevice:
    misc_deregister(&rollup->mdev);
free_rollup:
    kfree(rollup);
    return ret;
}

static int rollup_driver_remove(struct platform_device *pdev)
{
    struct rollup_device *rollup = platform_get_drvdata(pdev);
    misc_deregister(&rollup->mdev);
    kfree(rollup);
    dev_info(&pdev->dev, "unregistered\n");
    return 0;
}

static const struct of_device_id cartesi_rollup_match[] = {
    {.compatible = "ctsi-rollup",}, {},
};
MODULE_DEVICE_TABLE(of, cartesi_rollup_match);

static struct platform_driver rollup_driver = {
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = cartesi_rollup_match,
    },
    .probe = rollup_driver_probe,
    .remove = rollup_driver_remove,
};

module_platform_driver(rollup_driver);

MODULE_DESCRIPTION(MODULE_DESC);
MODULE_LICENSE("GPL");
