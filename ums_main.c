#include "ums.h"
// Module parameters                                                       
static bool disable_hub_lpm;
module_param(disable_hub_lpm, bool, 0444);
MODULE_PARM_DESC(disable_hub_lpm,"Forbid hub-initiated U1/U2 transitions (default: allowed)");

static bool force_bot;
module_param(force_bot, bool, 0444);
MODULE_PARM_DESC(force_bot,"Use BOT even when UAS is possible (for comparison)");

int ums_major;

DEFINE_IDA(ums_ida);

// USB device ID table                                                     
static const struct usb_device_id ums_table[] = {
    {USB_INTERFACE_INFO(USB_CLASS_MASS_STORAGE,UMS_SUBCLASS_SCSI,UMS_PROTO_UAS)},
    {USB_INTERFACE_INFO(USB_CLASS_MASS_STORAGE,UMS_SUBCLASS_SCSI,UMS_PROTO_BOT)},
    { }
};

MODULE_DEVICE_TABLE(usb, ums_table);

// Free device                                                             
static void ums_free_dev(struct ums_dev *dev)
{
    kfree(dev->bot_cbw);
    kfree(dev->bot_csw);
    kfree(dev->bot_buf);
    usb_put_dev(dev->udev);
    kfree(dev);
}

// USB probe                                                               
static int ums_probe(struct usb_interface *intf,const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);

    struct ums_dev *dev;

    int uas_alt;
    int bot_alt;

    int err;
    int i;

    bool try_uas;
    uas_alt = ums_find_altsetting(intf,UMS_PROTO_UAS);
    bot_alt = ums_find_altsetting(intf,UMS_PROTO_BOT);

    if(bot_alt < 0 && uas_alt < 0){
        return -ENODEV;
    }
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);

    if(!dev){
        return -ENOMEM;
    }
    dev->udev = usb_get_dev(udev);
    dev->intf = intf;
    dev->ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

    mutex_init(&dev->io_mutex);
    init_usb_anchor(&dev->anchor);
    atomic_set(&dev->disconnected, 0);

    atomic_set(&dev->inflight, 0);
    atomic_set(&dev->reset_pending, 0);
    dev->bot_tag = 1;
    
    dev_info(&intf->dev,"USB MS: ---- DESCRIPTORS ----\n");
    ums_dump_link_info(dev);
    // Decide whether to use UAS 
    try_uas = !force_bot && uas_alt >= 0 && udev->speed >= USB_SPEED_SUPER;

    if(uas_alt >= 0 && !try_uas){
        dev_info(&intf->dev,"USB Mass Storage : UAS available but not used (%s)\n", force_bot ? "force_bot=1" : "link is not SuperSpeed");
    }

    // UAS initialization                                                  
    if(try_uas){
        dev_info(&intf->dev,"USB MS: ---- UAS: ENDPOINTS + STREAMS ----\n");
        err = uas_enable(dev, uas_alt);
        if(!err){
            dev->mode = UMS_MODE_UAS;
        }else{
            dev_info(&intf->dev, "USB Mass Storage UAS setup failed (%d) \n",err );
            uas_free_streams(dev);
            try_uas = false;
        }
    }

    // BOT initialization                                                  
    if(!try_uas){
        if(bot_alt < 0){
            dev_err(&intf->dev,"USB Mass Storage : No BOT altsetting to fall back to\n");
            err = -ENODEV;
            goto err_free;
        }

        if(intf->cur_altsetting->desc.bAlternateSetting != bot_alt){
            err = usb_set_interface(udev, dev->ifnum,bot_alt);
            if(err){
                goto err_free;
            }    
        }

        dev_info(&intf->dev,"USB MS: ---- BOT_ENDPOINTS ----\n");
        err = bot_find_endpoints(dev,intf->cur_altsetting);
        if(err){
            goto err_free;
        }
        dev->mode = UMS_MODE_BOT;
    }
    dev_info(&intf->dev, "USB MS: mode = %s\n", dev->mode == UMS_MODE_UAS ? "UAS (SuperSpeed + streams)" : "BOT (Bulk-Only)" );
    usb_set_intfdata(intf, dev);
    // SCSI initialization                                                 
    dev_info(&intf->dev, "USB Mass Storage ---- INQUIRY----\n");
    err = ums_inquiry(dev);

    if(err){
        goto err_streams;
    }
    // Wait until the storage device becomes ready 
    for(i = 0; i < 5; i++){
        err = ums_test_unit_ready(dev);
        if(!err){
            break;
        }
        msleep(500);
    }

    if(err){
        dev_err(&intf->dev,"USB MS: TEST UNIT READY failed: %d\n",err);
        goto err_streams;
    }
    // Read device capacity 
    err = ums_read_capacity(dev);
    if(err){
        goto err_streams;
    }
    // Register block device                                               
    err = ums_setup_block_device(dev);
    if(err){
        goto err_streams;
    }
    dev_info(&intf->dev,"USB Mass Storage - PROBE COMPLETE - /dev/%s ready\n",dev->disk->disk_name);
    return 0;

err_streams:
    usb_set_intfdata(intf, NULL);
    uas_free_streams(dev);

err_free:
    uas_free_streams(dev);
    ums_free_dev(dev);
    return err;
}

// USB disconnect                                                          
static void ums_disconnect(struct usb_interface *intf)
{
    struct ums_dev *dev = usb_get_intfdata(intf);
    dev_info(&intf->dev,"USB Mass Storage-device disconnected\n");

    if(!dev){
        return;
    }
    atomic_set(&dev->disconnected, 1);

    if(dev->mode == UMS_MODE_UAS){
        usb_poison_anchored_urbs(&dev->anchor);
    }
    ums_remove_block_device(dev);
    uas_free_streams(dev);
    usb_set_intfdata(intf, NULL);
    ums_free_dev(dev);
}

// USB quiesce                                                            
static void ums_quiesce(struct ums_dev *dev)
{
    if(dev->disk){
        blk_mq_quiesce_queue(dev->disk->queue);
    }
    if(dev->mode == UMS_MODE_UAS){
        usb_kill_anchored_urbs(&dev->anchor);
    }    
}

// USB unquiesce                                                           
static void ums_unquiesce(struct ums_dev *dev)
{
    if(dev->disk){
        blk_mq_unquiesce_queue(dev->disk->queue);
    }    
}

// USB pre-reset                                                          
static int ums_pre_reset(struct usb_interface *intf)
{
    struct ums_dev *dev = usb_get_intfdata(intf);
    if(dev){
        ums_quiesce(dev);
    }
    return 0;
}

// USB post-reset                                                         
static int ums_post_reset(struct usb_interface *intf)
{
    struct ums_dev *dev = usb_get_intfdata(intf);

    if(dev){
        uas_restore(dev);
    }
    return 0;
}

// USB suspend                                                             
static int ums_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct ums_dev *dev = usb_get_intfdata(intf);
    if(!dev){
        return 0;
    }
    if(atomic_read(&dev->disconnected)){
        return -ENODEV;
    }
    if(PMSG_IS_AUTO(message) && atomic_read(&dev->inflight)){
        return -EBUSY;
    }
    ums_quiesce(dev);
    dev_info(&intf->dev, "USB MS: suspend (%s)\n",dev->udev->speed >= USB_SPEED_SUPER ? "link -> U3" : "bus suspend");
    return 0;
}

// USB resume                                                              
static int ums_resume(struct usb_interface *intf)
{
    struct ums_dev *dev = usb_get_intfdata(intf);
    if(!dev){
        return 0;
    }
    if(atomic_read(&dev->disconnected)){
        return -ENODEV;
    }
    dev_info(&intf->dev,"USB MS: resume\n" );

    ums_unquiesce(dev);

    return 0;
}

// USB reset resume                                                        
static int ums_reset_resume(struct usb_interface *intf)
{
    struct ums_dev *dev = usb_get_intfdata(intf);
    if(!dev){
        return 0;
    }
    if(atomic_read(&dev->disconnected)){
        return -ENODEV;
    }
    dev_info(&intf->dev,"USB MS: reset-resume\n");
    uas_restore(dev);
    return 0;
}

// USB driver                                                              
static struct usb_driver ums_driver = {
    .name = "usb_ms_combo",
    .id_table = ums_table,
    .probe = ums_probe,
    .disconnect = ums_disconnect,
    .suspend = ums_suspend,
    .resume = ums_resume,
    .reset_resume = ums_reset_resume,
    .pre_reset = ums_pre_reset,
    .post_reset = ums_post_reset,
    .supports_autosuspend = 1,
};

static int __init ums_init(void)
{
    int res;

    res = register_blkdev(0,"usb_ms_combo");

    if(res < 0){
        pr_err("USB MS: register_blkdev failed: %d\n", res);
        return res;
    }

    ums_major = res;
    ums_driver.disable_hub_initiated_lpm = disable_hub_lpm;
    res = usb_register(&ums_driver);
    if(res){
        pr_err("USB Mass Storage usb_register failed: %d\n",res);
        unregister_blkdev(ums_major, "usb_ms_combo");
        return res;
    }
    pr_info("USB MS: block major %d, hub LPM %s, force_bot=%d\n",ums_major,disable_hub_lpm ? "disabled" : "allowed",force_bot);
    return 0;
}
static void __exit ums_exit(void)
{
    usb_deregister(&ums_driver);
    unregister_blkdev(ums_major, "usb_ms_combo");
    ida_destroy(&ums_ida);
    pr_info( "USB mass storage driver unloaded\n");
}

module_init(ums_init);
module_exit(ums_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rahul");
MODULE_DESCRIPTION("USB Mass Storage");
