#include "ums.h"

// BOT endpoint detection                                                 
int bot_find_endpoints(struct ums_dev *dev,struct usb_host_interface *alt)
{
    struct usb_endpoint_descriptor *in = NULL, *out = NULL;
    int i;

    for(i = 0; i < alt->desc.bNumEndpoints; i++){
        struct usb_host_endpoint *ep = &alt->endpoint[i];

        dev_info(&dev->intf->dev,"USB ms : BOT ep 0x%02x attr 0x%02x maxpacket=%u bMaxBurst=%u\n",ep->desc.bEndpointAddress,ep->desc.bmAttributes,usb_endpoint_maxp(&ep->desc),ep->ss_ep_comp.bMaxBurst);

        if(!in && usb_endpoint_is_bulk_in(&ep->desc)){
            in = &ep->desc;
        }else if(!out && usb_endpoint_is_bulk_out(&ep->desc)){
            out = &ep->desc;
        }    
    }

    if(!in || !out){
        dev_err(&dev->intf->dev,"USB ms: BOT bulk endpoints not found\n");
        return -ENODEV;
    }

    dev->bot_in_pipe = usb_rcvbulkpipe(dev->udev, usb_endpoint_num(in));
    dev->bot_out_pipe = usb_sndbulkpipe(dev->udev, usb_endpoint_num(out));

    dev->bot_cbw = kzalloc(sizeof(*dev->bot_cbw), GFP_KERNEL);
    dev->bot_csw = kzalloc(sizeof(*dev->bot_csw), GFP_KERNEL);
    dev->bot_buf = kmalloc(UMS_MAX_XFER_BYTES, GFP_KERNEL);

    if(!dev->bot_cbw || !dev->bot_csw || !dev->bot_buf){
        return -ENOMEM;
    }
    return 0;
}

// BOT reset recovery                                                    
void bot_reset_recovery(struct ums_dev *dev)
{
    int r;

    dev_info(&dev->intf->dev,"USB ms: BOT reset recovery\n");

    r = usb_control_msg(dev->udev,usb_sndctrlpipe(dev->udev, 0),BOT_RESET_REQUEST,USB_TYPE_CLASS | USB_RECIP_INTERFACE,0,dev->ifnum,NULL,0,UMS_BOT_TIMEOUT);

    if(r < 0){
        dev_err(&dev->intf->dev,"USB ms: BOT reset request failed: %d\n", r);
    }
    usb_clear_halt(dev->udev, dev->bot_out_pipe);
    usb_clear_halt(dev->udev, dev->bot_in_pipe);
}

// BOT command transfer                                                    
// io_mutex must be held                                                  
int bot_xfer_locked(struct ums_dev *dev,const u8 *cdb,u8 cdb_len,void *buf, u32 len, bool data_in)
{
    struct bot_cbw *cbw = dev->bot_cbw;
    struct bot_csw *csw = dev->bot_csw;

    u32 tag = dev->bot_tag++;

    int xfer = 0;
    int err, data_err = 0;
    int tries;

    unsigned int pipe;

    if(cdb_len == 0 || cdb_len > 16 || (len && !buf)){
        return -EINVAL;
    }
    memset(cbw, 0, sizeof(*cbw));

    cbw->dCBWSignature = cpu_to_le32(BOT_CBW_SIGNATURE);

    cbw->dCBWTag = cpu_to_le32(tag);

    cbw->dCBWDataTransferLength = cpu_to_le32(len);
    cbw->bmCBWFlags = data_in ? BOT_FLAG_IN : BOT_FLAG_OUT;

    cbw->bCBWCBLength = cdb_len;
    memcpy(cbw->CBWCB, cdb, cdb_len);
    dev_dbg(&dev->intf->dev, "USB ms: BOT cmd 0x%02x tag=0x%08x len=%u\n",cdb[0], tag, len);

    // CBW stage 

    err = usb_bulk_msg(dev->udev, dev->bot_out_pipe, cbw,BOT_CBW_LENGTH, &xfer,UMS_BOT_TIMEOUT);
    if(err || xfer != BOT_CBW_LENGTH){
        dev_err(&dev->intf->dev, "USB ms: CBW failed: %d (%d bytes)\n", err, xfer);
        bot_reset_recovery(dev);
        return err ? err : -EIO;
    }

    // Data stage 

    if(len){
        pipe = data_in ? dev->bot_in_pipe : dev->bot_out_pipe;

        xfer = 0;

        err = usb_bulk_msg( dev->udev,pipe,buf,len,&xfer,UMS_BOT_TIMEOUT);

        if(err == -EPIPE){
            usb_clear_halt(dev->udev, pipe);
            data_err = -EIO;
        }
        else if(err){
            dev_err(&dev->intf->dev,"USB MS: data stage failed: %d\n", err);
            bot_reset_recovery(dev);
            return err;
        }
        else if(xfer != len){
            data_err = -EIO;
        }
    }

    // CSW stage 

    for(tries = 0; tries < 2; tries++){
        xfer = 0;

        err = usb_bulk_msg(dev->udev,dev->bot_in_pipe,csw,BOT_CSW_LENGTH, &xfer,UMS_BOT_TIMEOUT);

        if(err == -EPIPE){
            usb_clear_halt(dev->udev, dev->bot_in_pipe);
            continue;
        }

        break;
    }

    if (err || xfer != BOT_CSW_LENGTH) {
        dev_err(&dev->intf->dev,"USB MS: CSW failed: %d (%d bytes)\n",err, xfer);
        bot_reset_recovery(dev);
        return err ? err : -EIO;
    }

    // Validate CSW 
    if (le32_to_cpu(csw->dCSWSignature) != BOT_CSW_SIGNATURE || le32_to_cpu(csw->dCSWTag) != tag){
        dev_err(&dev->intf->dev,"USB MS: bad CSW signature/tag\n");
        bot_reset_recovery(dev);
        return -EIO;
    }

    switch(csw->bCSWStatus){

    case BOT_STATUS_PASS:
        return data_err;

    case BOT_STATUS_FAIL:
        dev_warn(&dev->intf->dev,"USB MS: BOT command 0x%02x failed\n", cdb[0]);
        return -EIO;

    case BOT_STATUS_PHASE_ERROR:
        dev_err(&dev->intf->dev,"USB MS: BOT phase error\n");

        bot_reset_recovery(dev);
        return -EPROTO;

    default:
        bot_reset_recovery(dev);
        return -EIO;
    }
}

// BOT synchronous command                                                
// Probe-time wrapper                                                      
int bot_sync_cmd(struct ums_dev *dev,const u8 *cdb,u8 cdb_len,void *buf, u32 len, bool data_in)
{
    int err;

    if(len > 512){
        return -EINVAL;
    }
    mutex_lock(&dev->io_mutex);

    if(!data_in && len){
        memcpy(dev->bot_buf, buf, len);
    }
    err = bot_xfer_locked(dev,cdb, cdb_len, len ? dev->bot_buf : NULL, len,data_in);
    if(!err && data_in && len){
        memcpy(buf, dev->bot_buf, len);
    }
    mutex_unlock(&dev->io_mutex);

    return err;
}

// BOT block request                                                       
blk_status_t bot_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct ums_dev *dev = rq->q->queuedata;

    bool write;

    u8 cdb[10];

    u32 lba, blocks, nbytes;

    int err;

    blk_mq_start_request(rq);

    if(atomic_read(&dev->disconnected)){
        goto fail;
    }
    switch(req_op(rq)){

    case REQ_OP_READ:
    case REQ_OP_WRITE:
        break;

    case REQ_OP_FLUSH:
        blk_mq_end_request(rq, BLK_STS_NOTSUPP);
        return BLK_STS_OK;

    default:
        blk_mq_end_request(rq, BLK_STS_NOTSUPP);
        return BLK_STS_OK;
    }

    if(!ums_map_rq(dev, rq, &lba, &blocks, &nbytes)){
        goto fail;
    }
    err = usb_autopm_get_interface(dev->intf);

    if(err){
        goto fail;
    }
    atomic_inc(&dev->inflight);
    write = req_op(rq) == REQ_OP_WRITE;

    ums_build_rw_cdb(cdb, write, lba, blocks);
    mutex_lock(&dev->io_mutex);

    if(write){
        ums_copy_from_request(rq, dev->bot_buf);
    }
    err = bot_xfer_locked(dev,cdb,10,dev->bot_buf,nbytes,!write);

    if(!err && !write){
        ums_copy_to_request(rq, dev->bot_buf);
    }
    mutex_unlock(&dev->io_mutex);
    atomic_dec(&dev->inflight);
    usb_autopm_put_interface(dev->intf);

    blk_mq_end_request( rq,err ? BLK_STS_IOERR : BLK_STS_OK );
    return BLK_STS_OK;

fail:
    blk_mq_end_request(rq, BLK_STS_IOERR);
    return BLK_STS_OK;
}

// BOT block MQ operations                                                
const struct blk_mq_ops bot_mq_ops = {
    .queue_rq = bot_queue_rq,
};

