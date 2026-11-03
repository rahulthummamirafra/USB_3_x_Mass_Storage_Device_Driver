#include "ums.h"

// Block device operations                                                
int ums_bd_open(struct gendisk *disk, blk_mode_t mode)
{
    struct ums_dev *dev = disk->private_data;
    return atomic_read(&dev->disconnected) ? -ENODEV : 0;
}

void ums_bd_release(struct gendisk *disk)
{
}

const struct block_device_operations ums_bd_ops = {
    .owner   = THIS_MODULE,
    .open    = ums_bd_open,
    .release = ums_bd_release,
};

// Block device setup                                                      
int ums_setup_block_device(struct ums_dev *dev)
{
    struct queue_limits lim = { };
    u64 sectors;
    int result;

    /* Allocate device index */
    dev->index = ida_alloc_max(&ums_ida, UMS_MAX_DEVICES - 1, GFP_KERNEL);
    if(dev->index < 0){
        dev_err(&dev->intf->dev, "USB MS: too many devices\n");
        return -ENOSPC;
    }

    /* Queue limits */
    lim.logical_block_size = dev->block_size;
    lim.physical_block_size = dev->block_size;
    lim.max_hw_sectors = min_t(u32, UMS_MAX_XFER_BYTES, dev->block_size * 128) >> 9;

    /* Tag set configuration */
    dev->tag_set.nr_hw_queues = 1;
    dev->tag_set.numa_node = NUMA_NO_NODE;
    dev->tag_set.flags = BLK_MQ_F_BLOCKING;
    dev->tag_set.driver_data = dev;

    if(dev->mode == UMS_MODE_UAS){
        dev->tag_set.ops = &uas_mq_ops;
        dev->tag_set.queue_depth = dev->qdepth;
        dev->tag_set.cmd_size = sizeof(struct uas_cmd);
        dev->tag_set.timeout = UAS_REQ_TIMEOUT;
    }else{
        dev->tag_set.ops = &bot_mq_ops;
        dev->tag_set.queue_depth = 1;
    }

    /* Allocate tag set */
    result = blk_mq_alloc_tag_set(&dev->tag_set);
    if(result){
        dev_err(&dev->intf->dev, "USB MS: blk_mq_alloc_tag_set failed: %d\n", result);
        goto err_ida;
    }

    /* Allocate gendisk */
    dev->disk = blk_mq_alloc_disk(&dev->tag_set, &lim, dev);
    if(IS_ERR(dev->disk)){
        result = PTR_ERR(dev->disk);
        dev->disk = NULL;
        dev_err(&dev->intf->dev, "USB MS: blk_mq_alloc_disk failed: %d\n", result);
        goto err_tag_set;
    }

    /* Configure disk */
    dev->disk->major = ums_major;
    dev->disk->first_minor = dev->index * UMS_MINORS;
    dev->disk->minors = UMS_MINORS;
    dev->disk->fops = &ums_bd_ops;
    dev->disk->private_data = dev;

    snprintf(dev->disk->disk_name, sizeof(dev->disk->disk_name), "usbms%d", dev->index);

    /* Set capacity */
    sectors = div_u64(dev->capacity, 512);
    set_capacity(dev->disk, sectors);

    /* Register disk */
    result = device_add_disk(&dev->intf->dev, dev->disk, NULL);
    if(result){
        dev_err(&dev->intf->dev, "USB MS: device_add_disk failed: %d\n", result);
        goto err_put_disk;
    }

    dev_info(&dev->intf->dev,"USB MS: /dev/%s registered (%llu sectors, %u byte blocks, %s)\n",dev->disk->disk_name,sectors,dev->block_size,dev->mode == UMS_MODE_UAS ? "UAS" : "BOT");

    return 0;

err_put_disk:
    put_disk(dev->disk);
    dev->disk = NULL;

err_tag_set:
    blk_mq_free_tag_set(&dev->tag_set);

err_ida:
    ida_free(&ums_ida, dev->index);
    return result;
}

// Block device removal                                                   
void ums_remove_block_device(struct ums_dev *dev)
{
    if(!dev->disk){
        return;
    }
    del_gendisk(dev->disk);
    put_disk(dev->disk);
    
    // UAS:exit_request() frees the per-request URBs and buffers
    blk_mq_free_tag_set(&dev->tag_set);
    ida_free(&ums_ida, dev->index);
    dev->disk = NULL;
}

