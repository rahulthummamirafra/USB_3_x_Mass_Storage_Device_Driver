#include "ums.h"

// UAS endpoint descriptor parsing                                         
static int uas_pipe_id(struct usb_host_endpoint *ep)
{
	unsigned char *extra = ep->extra;
	int len = ep->extralen;
	while(len >= 4){
		if(extra[0] < 4 || extra[0] > len) break;

		if(extra[1] == UAS_DT_PIPE_USAGE){
			return extra[2];
		}

		len -= extra[0];
		extra += extra[0];
	}
	return -ENOENT;
}

// Maximum supported streams                                               
static unsigned int uas_ep_max_streams(struct usb_host_endpoint *ep)
{
	u8 e = ep->ss_ep_comp.bmAttributes & 0x1f;
	return e ? (1U << e) : 0;
}

// Find UAS endpoints                                                      
int uas_find_endpoints(struct ums_dev *dev,struct usb_host_interface *alt)
{
	struct usb_device *udev = dev->udev;
	int i;
	int id;
	memset(dev->eps, 0, sizeof(dev->eps));
	for(i = 0; i < alt->desc.bNumEndpoints; i++){
		struct usb_host_endpoint *ep = &alt->endpoint[i];
		id = uas_pipe_id(ep);

		if(id < UAS_PIPE_CMD || id > UAS_PIPE_DATA_OUT || dev->eps[id]) continue;
		dev->eps[id] = ep;
		dev_info(&dev->intf->dev, "USB MS: UAS pipe %d = ep 0x%02x maxpacket=%u bMaxBurst=%u max_streams=%u\n",id, ep->desc.bEndpointAddress,usb_endpoint_maxp(&ep->desc),ep->ss_ep_comp.bMaxBurst,uas_ep_max_streams(ep));
	}

	/* Check endpoint availability */

	for(id = UAS_PIPE_CMD; id <= UAS_PIPE_DATA_OUT;id++){
		if(!dev->eps[id] || !usb_endpoint_xfer_bulk(&dev->eps[id]->desc)){
			dev_err(&dev->intf->dev,"USB Mass_Storage - UAS pipe %d missing / not bulk\n",id);
			return -ENODEV;
		}
	}

	/* Check endpoint directions */
	if(!usb_endpoint_dir_out(&dev->eps[UAS_PIPE_CMD]->desc) ||!usb_endpoint_dir_in(&dev->eps[UAS_PIPE_STATUS]->desc) ||!usb_endpoint_dir_in(&dev->eps[UAS_PIPE_DATA_IN]->desc) ||!usb_endpoint_dir_out(&dev->eps[UAS_PIPE_DATA_OUT]->desc)){
		dev_err(&dev->intf->dev,"USB Mass_Storage - UAS endpoint directions wrong\n");
		return -ENODEV;
	}

	/* Check stream support */
	for(id = UAS_PIPE_STATUS;id <= UAS_PIPE_DATA_OUT;id++){
		if(uas_ep_max_streams(dev->eps[id]) < 2){
			dev_err(&dev->intf->dev,"USB MS: UAS pipe %d has no streams\n",id);
			return -ENODEV;
		}
	}

	/* Build USB pipes */
	dev->cmd_pipe = usb_sndbulkpipe(udev,usb_endpoint_num(&dev->eps[UAS_PIPE_CMD]->desc));
	dev->status_pipe = usb_rcvbulkpipe(udev,usb_endpoint_num(&dev->eps[UAS_PIPE_STATUS]->desc));
	dev->data_in_pipe = usb_rcvbulkpipe(udev,usb_endpoint_num(&dev->eps[UAS_PIPE_DATA_IN]->desc));
	dev->data_out_pipe = usb_sndbulkpipe(udev, usb_endpoint_num(&dev->eps[UAS_PIPE_DATA_OUT]->desc));
	return 0;
}

// Allocate UAS streams                                                     
int uas_alloc_streams(struct ums_dev *dev)
{
	struct usb_host_endpoint *eps[3] = {
		dev->eps[UAS_PIPE_STATUS],
		dev->eps[UAS_PIPE_DATA_IN],
		dev->eps[UAS_PIPE_DATA_OUT],
	};

	int r;
	r = usb_alloc_streams(dev->intf,eps,3,UAS_REQ_STREAMS,GFP_KERNEL);
	if(r < 0){
		dev_err(&dev->intf->dev,"USB MS: usb_alloc_streams failed: %d\n",r);
		return r;
	}

	if(r < 2){
		usb_free_streams(dev->intf,eps,3,GFP_KERNEL);
		return -ENODEV;
	}

	dev->streams = r;
	dev->qdepth = min_t(unsigned int, r - 1,UAS_MAX_QDEPTH);
	dev->streams_on = true;    
	dev_info(&dev->intf->dev,"USB Mass Storage - %d streams allocated, queue depth %u\n",r,dev->qdepth);
	return 0;
}

// Free UAS streams                                                        
void uas_free_streams(struct ums_dev *dev){
	struct usb_host_endpoint *eps[3] = {
		dev->eps[UAS_PIPE_STATUS],
		dev->eps[UAS_PIPE_DATA_IN],
		dev->eps[UAS_PIPE_DATA_OUT],
	};

	if(!dev->streams_on){
		return;
	}
	usb_free_streams(dev->intf,eps,3,GFP_KERNEL);
	dev->streams_on = false;
}

// Enable UAS                                                              
int uas_enable(struct ums_dev *dev, int uas_alt)
{
	int err;
	err = usb_set_interface(dev->udev,dev->ifnum,uas_alt);

	if(err){
		dev_err( &dev->intf->dev,"USB MS: usb_set_interface(alt %d) failed: %d\n",uas_alt,err);
		return err;
	}
	err = uas_find_endpoints(dev,dev->intf->cur_altsetting);
	if(err){
		return err;
	}
	return uas_alloc_streams(dev);
}

// UAS command cleanup                                                   
static void uas_cmd_free(struct uas_cmd *cmd)
{
	usb_free_urb(cmd->cmd_urb);
	usb_free_urb(cmd->status_urb);
	usb_free_urb(cmd->data_urb);

	kfree(cmd->cmd_iu);
	kfree(cmd->sense_iu);
	kfree(cmd->bounce);
}

// Allocate UAS command resources                                          
static int uas_cmd_alloc(struct ums_dev *dev,struct uas_cmd *cmd,u32 bounce_size)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->dev = dev;
	spin_lock_init(&cmd->lock);
	cmd->cmd_urb = usb_alloc_urb(0,GFP_KERNEL);
	cmd->status_urb = usb_alloc_urb(0,GFP_KERNEL);

	cmd->data_urb = usb_alloc_urb(0,GFP_KERNEL);
	cmd->cmd_iu = kzalloc(sizeof(*cmd->cmd_iu),GFP_KERNEL);

	cmd->sense_iu = kzalloc(sizeof(*cmd->sense_iu),GFP_KERNEL);
	cmd->bounce = kmalloc(bounce_size,GFP_KERNEL);

	cmd->bounce_size = bounce_size;

	if(!cmd->cmd_urb || !cmd->status_urb || !cmd->data_urb || !cmd->cmd_iu || !cmd->sense_iu || !cmd->bounce){
		uas_cmd_free(cmd);
		return -ENOMEM;
	}
	return 0;
}

// Handle UAS transport errors                                             
void uas_note_error(struct ums_dev *dev, int err)
{
	switch(err){
		case -EPIPE:
		case -EPROTO:
		case -EOVERFLOW:
		case -EILSEQ:
		case -ETIMEDOUT:

			if(!atomic_xchg(&dev->reset_pending, 1)){
				dev_err(&dev->intf->dev,"USB MS: UAS transport error %d, " "queueing device reset\n",err);
				usb_queue_reset_device(dev->intf);
			}
			break;
		default:
			break;
	}
}

// Finish UAS command                                                      
static void uas_finish(struct uas_cmd *cmd)
{
	struct ums_dev *dev = cmd->dev;
	struct request *rq = cmd->rq;

	blk_status_t sts = cmd->error ? BLK_STS_IOERR : BLK_STS_OK;

	if(!rq){
		complete(cmd->done);
		return;
	}

	if(!cmd->error && cmd->data_in){
		ums_copy_to_request(rq, cmd->bounce);
	}
	atomic_dec(&dev->inflight);
	usb_autopm_put_interface_async(dev->intf);
	blk_mq_end_request(rq, sts);
}

// Check whether UAS command is completed                                  
// cmd->lock must be held                                                 
static bool uas_check_done_locked(struct uas_cmd *cmd)
{
	if(cmd->pending || cmd->finishing){
		return false;
	}
	cmd->finishing = true;
	return true;
}

// Fail UAS command                                                       
// cmd->lock must be held                                                  
static bool uas_fail_locked(struct uas_cmd *cmd, int err)
{
	if(!cmd->error){
		cmd->error = err;
	}
	if(cmd->data_expected && !cmd->data_submitted){
		cmd->data_expected = false;
		cmd->pending--;
	}

	usb_unlink_urb(cmd->cmd_urb);
	usb_unlink_urb(cmd->status_urb);
	if(cmd->data_submitted){
		usb_unlink_urb(cmd->data_urb);
	}
	uas_note_error(cmd->dev, err);
	return uas_check_done_locked(cmd);
}

// Submit anchored URB                                                     
static int uas_submit_urb(struct ums_dev *dev,struct urb *urb, gfp_t gfp)
{
	int err;
	usb_anchor_urb(urb,&dev->anchor);
	err = usb_submit_urb(urb,gfp);

	if(err){
		usb_unanchor_urb(urb);
	}
	return err;
}

// Command URB completion                                                  
static void uas_cmd_complete(struct urb *urb)
{
	struct uas_cmd *cmd = urb->context;
	unsigned long flags;

	bool done;
	spin_lock_irqsave(&cmd->lock,flags);
	cmd->pending--;
	if(urb->status){
		done = uas_fail_locked(cmd,urb->status);
	}else if(urb->actual_length != sizeof(struct uas_cmd_iu)){
		done = uas_fail_locked(cmd,-EIO);
	}else{
		done = uas_check_done_locked(cmd);
	}

	spin_unlock_irqrestore(&cmd->lock,flags);

	if(done){
		uas_finish(cmd);
	}
}

// Data URB completion                                                     
static void uas_data_complete(struct urb *urb)
{
	struct uas_cmd *cmd = urb->context;
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&cmd->lock,flags);
	cmd->pending--;

	if(urb->status){
		done = uas_fail_locked(cmd,urb->status);
	}else if(urb->actual_length != cmd->nbytes){
		done = uas_fail_locked(cmd,-EIO);
	}else{
		done = uas_check_done_locked(cmd);
	}

	spin_unlock_irqrestore(&cmd->lock,flags);
	if(done){
		uas_finish(cmd);
	}
}

// SCSI sense information                                                 
static void uas_log_sense(struct uas_cmd *cmd)
{
	struct uas_sense_iu *iu = cmd->sense_iu;
	if(be16_to_cpu(iu->len) >= 14){
		dev_warn(&cmd->dev->intf->dev,"USB MS: SCSI status 0x%02x sense key 0x%x ASC 0x%02x ASCQ 0x%02x\n",iu->status, iu->sense[2] & 0x0f, iu->sense[12],iu->sense[13]);
	}else{
		dev_warn(&cmd->dev->intf->dev,"USB MS: SCSI status 0x%02x\n", iu->status);
	}
}

// UAS status URB completion                                              
static void uas_status_complete(struct urb *urb)
{
	struct uas_cmd *cmd = urb->context;
	struct ums_dev *dev = cmd->dev;
	struct uas_sense_iu *iu = cmd->sense_iu;

	unsigned long flags;
	bool done = false;
	int err;

	spin_lock_irqsave(&cmd->lock, flags);

	if (urb->status) {
		cmd->pending--;
		done = uas_fail_locked(cmd, urb->status);
		goto out;
	}

	if (be16_to_cpu(iu->tag) != cmd->stream) {
		cmd->pending--;
		done = uas_fail_locked(cmd, -EPROTO);
		goto out;
	}

	switch (iu->iu_id) {
		case UAS_IU_READ_READY:
		case UAS_IU_WRITE_READY:
			if(!cmd->data_expected ||!cmd->data_submitted ||(iu->iu_id == UAS_IU_READ_READY) != cmd->data_in){
				cmd->pending--;
				done = uas_fail_locked(cmd, -EPROTO);
				break;
			}

			/* Re-arm status URB for Sense IU */
			err = uas_submit_urb(dev, urb, GFP_ATOMIC);
			if(err){
				cmd->pending--;
				done = uas_fail_locked(cmd, err);
			}
			break;

		case UAS_IU_SENSE:
			cmd->pending--;
			if(iu->status){
				uas_log_sense(cmd);
				done = uas_fail_locked(cmd, -EIO);
			} else if(cmd->data_expected &&
					!cmd->data_submitted) {
				done = uas_fail_locked(cmd, -EIO);
			} else{
				done = uas_check_done_locked(cmd);
			}
			break;

		default:
			cmd->pending--;
			done = uas_fail_locked(cmd, -EPROTO);
			break;
	}

out:
	spin_unlock_irqrestore(&cmd->lock, flags);

	if(done){
		uas_finish(cmd);
	}

}

// Submit UAS command                                                      
static int uas_submit(struct uas_cmd *cmd, const u8 *cdb, u8 cdb_len, u32 nbytes, bool data_in, u16 stream, gfp_t gfp){
	struct ums_dev *dev = cmd->dev;
	struct uas_cmd_iu *iu = cmd->cmd_iu;
	unsigned long flags;

	bool done;
	int err;

	cmd->stream = stream;
	cmd->nbytes = nbytes;
	cmd->data_in = data_in;
	cmd->data_expected = nbytes != 0;
	cmd->data_submitted = false;

	cmd->finishing = false;
	cmd->error = 0;
	cmd->pending = 2 + (nbytes ? 1 : 0);

	/* Prepare command IU */
	memset(iu, 0, sizeof(*iu));
	iu->iu_id = UAS_IU_COMMAND;
	iu->tag = cpu_to_be16(stream);

	memcpy(iu->cdb, cdb, cdb_len);

	memset(cmd->sense_iu, 0, sizeof(*cmd->sense_iu));

	/* Command URB */
	usb_fill_bulk_urb(cmd->cmd_urb, dev->udev, dev->cmd_pipe, iu, sizeof(*iu), uas_cmd_complete, cmd);

	/* Status URB */
	usb_fill_bulk_urb(cmd->status_urb, dev->udev, dev->status_pipe, cmd->sense_iu, sizeof(*cmd->sense_iu), uas_status_complete, cmd);


	cmd->status_urb->stream_id = stream;

	/* Data URB */

	if(nbytes){
		usb_fill_bulk_urb(cmd->data_urb, dev->udev, data_in ? dev->data_in_pipe : dev->data_out_pipe, cmd->bounce, nbytes, uas_data_complete, cmd);
		cmd->data_urb->stream_id = stream;
	}


	/*
	 * Submission order:
	 * 1. Status URB
	 * 2. Data URB
	 * 3. Command URB
	 */
	err = uas_submit_urb(dev, cmd->status_urb, gfp);

	if(err){
		return err;
	}
	/* Submit data URB */
	if(nbytes){
		err = uas_submit_urb(dev, cmd->data_urb, gfp);
		if(err){
			spin_lock_irqsave(&cmd->lock, flags);
			cmd->pending--;
			done = uas_fail_locked(cmd, err);
			spin_unlock_irqrestore(&cmd->lock, flags);
			if(done){
				uas_finish(cmd);
			}
			return 0;
		}
		cmd->data_submitted = true;
	}


	/* Submit command URB */
	err = uas_submit_urb(dev, cmd->cmd_urb, gfp);
	if(err){
		spin_lock_irqsave(&cmd->lock, flags);
		cmd->pending--;
		done = uas_fail_locked(cmd, err);
		spin_unlock_irqrestore(&cmd->lock, flags);
		if(done){
			uas_finish(cmd);
		}
	}


	return 0;
}

// Synchronous UAS command                                                
int uas_sync_cmd(struct ums_dev *dev, const u8 *cdb, u8 cdb_len, void *buf, u32 len, bool data_in){
	DECLARE_COMPLETION_ONSTACK(done);
	struct uas_cmd *cmd;
	unsigned long flags;
	bool fin;
	int err;
	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if(!cmd){
		return -ENOMEM;
	}
	err = uas_cmd_alloc(dev, cmd, max_t(u32, len, 64));
	if(err){
		kfree(cmd);
		return err;
	}
	cmd->done = &done;
	if(!data_in && len){
		memcpy(cmd->bounce, buf, len);
	}
	err = uas_submit(cmd, cdb, cdb_len, len, data_in, UAS_SYNC_STREAM, GFP_KERNEL);


	if(!err){

		if(!wait_for_completion_timeout(&done,msecs_to_jiffies(UAS_SYNC_TIMEOUT_MS))){

			spin_lock_irqsave(&cmd->lock,flags);
			fin = uas_fail_locked(cmd,-ETIMEDOUT);
			spin_unlock_irqrestore(&cmd->lock,flags);

			if(fin){
				uas_finish(cmd);
			}
			wait_for_completion(&done);
		}

		err = cmd->error;

		if(!err && data_in && len){
			memcpy(buf, cmd->bounce, len);
		}
	}

	uas_cmd_free(cmd);

	kfree(cmd);

	return err;
}

// UAS block request                                                      
static blk_status_t uas_queue_rq(struct blk_mq_hw_ctx *hctx,const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;

	struct ums_dev *dev = rq->q->queuedata;

	struct uas_cmd *cmd =
		blk_mq_rq_to_pdu(rq);

	bool write;

	u8 cdb[10];

	u32 lba;
	u32 blocks;
	u32 nbytes;

	int err;

	blk_mq_start_request(rq);

	if(atomic_read(&dev->disconnected)){
		goto fail;
	}
	switch (req_op(rq)) {
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
	cmd->rq = rq;

	if(write){
		ums_copy_from_request(rq,cmd->bounce);
	}
	dev_dbg(&dev->intf->dev, "USB MS: UAS %s lba=%u blocks=%u stream=%d\n", write ? "WRITE" : "READ", lba, blocks, rq->tag + 1);


	err = uas_submit(cmd, cdb, 10, nbytes, !write, rq->tag + 1, GFP_KERNEL);


	if(err){
		atomic_dec(&dev->inflight);
		usb_autopm_put_interface(dev->intf);
		goto fail;
	}
	return BLK_STS_OK;
fail:
	blk_mq_end_request(rq,BLK_STS_IOERR);
	return BLK_STS_OK;
}

// UAS request timeout                                                     
static enum blk_eh_timer_return uas_timeout(struct request *rq)
{
	struct uas_cmd *cmd =  blk_mq_rq_to_pdu(rq);
	unsigned long flags;

	bool done;
	bool fin;

	dev_err(&cmd->dev->intf->dev,"USB MS: UAS request timed out (stream %u)\n",cmd->stream);
	spin_lock_irqsave(&cmd->lock,flags);

	done = uas_fail_locked(cmd,-ETIMEDOUT);
	fin = cmd->finishing;
	spin_unlock_irqrestore(&cmd->lock,flags);

	if(done){
		uas_finish(cmd);
	}
	return fin ? BLK_EH_DONE : BLK_EH_RESET_TIMER;
}

// Initialize UAS request                                                  
static int uas_init_request(struct blk_mq_tag_set *set,struct request *rq,unsigned int hctx_idx,unsigned int numa_node)
{
	return uas_cmd_alloc(set->driver_data,blk_mq_rq_to_pdu(rq),UMS_MAX_XFER_BYTES);
}

// Exit UAS request                                                        
static void uas_exit_request(struct blk_mq_tag_set *set,struct request *rq,unsigned int hctx_idx)
{
	uas_cmd_free(blk_mq_rq_to_pdu(rq));
}

// UAS block MQ operations                                                 
const struct blk_mq_ops uas_mq_ops = {
	.queue_rq = uas_queue_rq,
	.timeout = uas_timeout,
	.init_request = uas_init_request,
	.exit_request = uas_exit_request,
};

// Restore UAS after USB reset                                             
void uas_restore(struct ums_dev *dev)
{
	if(dev->mode == UMS_MODE_UAS){
		unsigned int old_depth = dev->disk ? dev->tag_set.queue_depth : 0;
		dev->streams_on = false;
		if(uas_alloc_streams(dev) || dev->qdepth < old_depth){
			dev_err(&dev->intf->dev,"USB MS: stream re-allocation failed,""taking device offline\n");
			atomic_set(&dev->disconnected,1);
		}

		atomic_set(&dev->reset_pending,0);
	}

	if(dev->disk){
		blk_mq_unquiesce_queue(dev->disk->queue);
	}
}

