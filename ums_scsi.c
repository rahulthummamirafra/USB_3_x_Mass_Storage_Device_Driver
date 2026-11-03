#include "ums.h"

// Transport-independent SCSI command                                    
int ums_sync_cmd(struct ums_dev *dev,const u8 *cdb,u8 cdb_len,void *buf, u32 len,bool data_in)
{
    if(dev->mode == UMS_MODE_UAS){
        return uas_sync_cmd(dev, cdb, cdb_len,buf, len, data_in);
    }
    return bot_sync_cmd(dev, cdb, cdb_len,buf, len, data_in);
}

// SCSI INQUIRY                                                            
int ums_inquiry(struct ums_dev *dev)
{
    u8 cdb[6] = {SCSI_CMD_INQUIRY,0,0,0,SCSI_INQUIRY_LENGTH, 0};

    u8 data[SCSI_INQUIRY_LENGTH + 1] = { 0 };
    int err;
    err = ums_sync_cmd(dev,cdb,6,data,SCSI_INQUIRY_LENGTH,true);
    if(err){
        dev_err(&dev->intf->dev,"USB MS: INQUIRY failed: %d\n",err);
        return err;
    }
    dev_info(&dev->intf->dev,"USB Mass_Storage: Vendor: %.8s Product: %.16s Rev: %.4s\n",&data[8],&data[16],&data[32]);
    return 0;
}
// SCSI TEST UNIT READY                                                   
int ums_test_unit_ready(struct ums_dev *dev)
{
    u8 cdb[6] = {
        SCSI_CMD_TEST_UNIT_READY
    };
    return ums_sync_cmd(dev,cdb,6,NULL,0,false);
}

// SCSI READ CAPACITY(10)                                                
int ums_read_capacity(struct ums_dev *dev)
{
    u8 cdb[10] = {
        SCSI_CMD_READ_CAPACITY10
    };

    u8 data[SCSI_READ_CAP_LENGTH];

    u32 last_lba;
    u32 bs;

    int err;
    err = ums_sync_cmd(dev,cdb,10,data,SCSI_READ_CAP_LENGTH,true);
    if(err){
        dev_err(&dev->intf->dev,"USB MS: READ CAPACITY failed: %d\n",err);
        return err;
    }
    last_lba = get_unaligned_be32(&data[0]);
    bs = get_unaligned_be32(&data[4]);

    if(!bs || bs < 512 || bs > 4096 || (bs & (bs - 1)) || bs % 512 || last_lba == 0xFFFFFFFFU){
        dev_err(&dev->intf->dev,"USB MS: unsupported capacity (block size=%u, last LBA=0x%x)\n",bs,last_lba);
        return -EOPNOTSUPP;
    }
    dev->last_lba = last_lba;
    dev->block_size = bs;
    dev->capacity =  ((u64)last_lba + 1) * bs;
    dev_info(&dev->intf->dev,"USB MS: last LBA=%u block size=%u capacity=%llu bytes\n",last_lba,bs,dev->capacity);
    return 0;
}

