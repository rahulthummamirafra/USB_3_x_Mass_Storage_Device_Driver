#include "ums.h"

// Find interface alternate setting                                       
int ums_find_altsetting(struct usb_interface *intf, u8 proto)
{
    int i;
    for(i = 0; i < intf->num_altsetting; i++){
        struct usb_host_interface *alt = &intf->altsetting[i];

        if(alt->desc.bInterfaceClass == USB_CLASS_MASS_STORAGE && alt->desc.bInterfaceSubClass == UMS_SUBCLASS_SCSI && alt->desc.bInterfaceProtocol == proto) {
            return alt->desc.bAlternateSetting;
        }
    }
    return -ENODEV;
}

// Copy data from USB buffer to block request                              
void ums_copy_to_request(struct request *rq, const void *src)
{
    struct bio_vec bvec;
    struct req_iterator iter;

    rq_for_each_segment(bvec, rq, iter){
        void *p = kmap_local_page(bvec.bv_page);
        memcpy(p + bvec.bv_offset,src,bvec.bv_len);
        kunmap_local(p);
        src += bvec.bv_len;
    }
}

// Copy data from block request to USB buffer                             
void ums_copy_from_request(struct request *rq, void *dst)
{
    struct bio_vec bvec;
    struct req_iterator iter;

    rq_for_each_segment(bvec, rq, iter){
        void *p = kmap_local_page(bvec.bv_page);
        memcpy(dst,p + bvec.bv_offset, bvec.bv_len);
        kunmap_local(p);
        dst += bvec.bv_len;
    }
}

// Validate block request                                                  
bool ums_map_rq(struct ums_dev *dev, struct request *rq, u32 *lba, u32 *blocks, u32 *nbytes)
{
    u64 byte_pos;
    u64 lba64;
    u32 n;

    byte_pos = (u64)blk_rq_pos(rq) << 9;
    n = blk_rq_bytes(rq);

    if(!dev->block_size || !n || n % dev->block_size || byte_pos % dev->block_size || n > UMS_MAX_XFER_BYTES){
        return false;
    }
    lba64 = div_u64(byte_pos, dev->block_size);

    if(lba64 > U32_MAX){
        return false;
    }
    *lba = (u32)lba64;
    *blocks = n / dev->block_size;
    *nbytes = n;

    return *blocks && *blocks <= 0xffff && (u64)*lba + *blocks <= (u64)dev->last_lba + 1;
}


// Build SCSI READ(10) / WRITE(10) CDB                                    
void ums_build_rw_cdb(u8 *cdb,bool write,u32 lba,u32 blocks)
{
    memset(cdb, 0, 10);

    cdb[0] = write ? SCSI_CMD_WRITE_10 : SCSI_CMD_READ_10;
    put_unaligned_be32(lba, &cdb[2]);
    put_unaligned_be16(blocks, &cdb[7]);
}

// USB link information                                                    
void ums_dump_link_info(struct ums_dev *dev)
{
    struct usb_device *udev = dev->udev;
    struct device *d = &dev->intf->dev;

    dev_info(d, "USB MS: link speed = %s\n",usb_speed_string(udev->speed));

    if(udev->speed < USB_SPEED_SUPER){
        return;
    }
    if(udev->bos && udev->bos->ss_cap){

        struct usb_ss_cap_descriptor *c = udev->bos->ss_cap;

        dev_info(d, "USB MS: BOS SS cap: speeds=0x%04x func_support=%u U1 exit=%u us U2 exit=%u us\n", le16_to_cpu(c->wSpeedSupported), c->bFunctionalitySupport, c->bU1devExitLat, le16_to_cpu(c->bU2DevExitLat));
    }else{
        dev_info(d,"USB MS: no BOS SuperSpeed capability descriptor\n");
    }

    dev_info(d,"USB MS: LPM capable=%u U1 enabled=%u U2 enabled=%u\n",udev->lpm_capable,udev->usb3_lpm_u1_enabled,udev->usb3_lpm_u2_enabled);
}

