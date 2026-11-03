#ifndef UMS_H
#define UMS_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/byteorder/generic.h>
#include <linux/unaligned.h>
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/math64.h>

// Module information                                                      
#define UMS_SUBCLASS_SCSI        0x06
#define UMS_PROTO_BOT            0x50
#define UMS_PROTO_UAS            0x62

// BOT 
#define BOT_CBW_SIGNATURE        0x43425355U
#define BOT_CSW_SIGNATURE        0x53425355U
#define BOT_CBW_LENGTH           31
#define BOT_CSW_LENGTH           13
#define BOT_RESET_REQUEST        0xFF
#define BOT_FLAG_OUT             0x00
#define BOT_FLAG_IN              0x80
#define BOT_STATUS_PASS          0x00
#define BOT_STATUS_FAIL          0x01
#define BOT_STATUS_PHASE_ERROR   0x02
#define UMS_BOT_TIMEOUT          5000

// UAS 
#define UAS_DT_PIPE_USAGE        0x24
#define UAS_PIPE_CMD             1
#define UAS_PIPE_STATUS          2
#define UAS_PIPE_DATA_IN         3
#define UAS_PIPE_DATA_OUT        4

#define UAS_IU_COMMAND           0x01
#define UAS_IU_SENSE             0x03
#define UAS_IU_RESPONSE          0x04
#define UAS_IU_TASK_MGMT         0x05
#define UAS_IU_READ_READY        0x06
#define UAS_IU_WRITE_READY       0x07

#define UAS_REQ_STREAMS          32
#define UAS_MAX_QDEPTH           32
#define UAS_SYNC_STREAM          1
#define UAS_SYNC_TIMEOUT_MS      5000
#define UAS_REQ_TIMEOUT          (10 * HZ)

// SCSI 
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_INQUIRY         0x12
#define SCSI_CMD_READ_CAPACITY10 0x25
#define SCSI_CMD_READ_10         0x28
#define SCSI_CMD_WRITE_10        0x2A

#define SCSI_INQUIRY_LENGTH      36
#define SCSI_READ_CAP_LENGTH     8

#define UMS_MAX_XFER_BYTES       (64 * 1024)
#define UMS_MAX_DEVICES          16
#define UMS_MINORS               16

enum ums_mode {
    UMS_MODE_BOT,
    UMS_MODE_UAS
};

extern int ums_major;
extern struct ida ums_ida;

// BOT structures                                                         
struct bot_cbw {
    __le32 dCBWSignature;
    __le32 dCBWTag;
    __le32 dCBWDataTransferLength;
    u8 bmCBWFlags;
    u8 bCBWLUN;
    u8 bCBWCBLength;
    u8 CBWCB[16];
} __attribute__((packed));

struct bot_csw {
    __le32 dCSWSignature;
    __le32 dCSWTag;
    __le32 dCSWDataResidue;
    u8 bCSWStatus;
} __attribute__((packed));

// UAS structures                                                          
struct uas_cmd_iu {
    u8 iu_id;
    u8 rsvd1;
    __be16 tag;
    u8 prio_attr;
    u8 rsvd5;
    u8 len;
    u8 rsvd7;
    u8 lun[8];
    u8 cdb[16];
} __attribute__((packed));

struct uas_sense_iu {
    u8 iu_id;
    u8 rsvd1;
    __be16 tag;
    __be16 status_qual;
    u8 status;
    u8 rsvd7[7];
    __be16 len;
    u8 sense[96];
} __attribute__((packed));

static_assert(sizeof(struct bot_cbw) == BOT_CBW_LENGTH);
static_assert(sizeof(struct bot_csw) == BOT_CSW_LENGTH);
static_assert(sizeof(struct uas_cmd_iu) == 32);
static_assert(sizeof(struct uas_sense_iu) == 112);

struct ums_dev {
    struct usb_device *udev;
    struct usb_interface *intf;

    enum ums_mode mode;

    u8 ifnum;

    // BOT 
    unsigned int bot_in_pipe;
    unsigned int bot_out_pipe;

    struct bot_cbw *bot_cbw;
    struct bot_csw *bot_csw;

    void *bot_buf;

    u32 bot_tag;

    struct mutex io_mutex;

    //UAS 
    struct usb_host_endpoint *eps[UAS_PIPE_DATA_OUT + 1];
    unsigned int cmd_pipe;
    unsigned int status_pipe;
    unsigned int data_in_pipe;
    unsigned int data_out_pipe;

    int streams;
    unsigned int qdepth;

    bool streams_on;

    struct usb_anchor anchor;

    atomic_t reset_pending;


    u32 last_lba;
    u32 block_size;

    u64 capacity;

    struct gendisk *disk;

    struct blk_mq_tag_set tag_set;

    int index;

    atomic_t disconnected;
    atomic_t inflight;
};

// UAS command context                                                     
struct uas_cmd {
    struct ums_dev *dev;

    struct request *rq;

    struct completion *done;

    spinlock_t lock;

    struct urb *cmd_urb;
    struct urb *status_urb;
    struct urb *data_urb;

    struct uas_cmd_iu *cmd_iu;
    struct uas_sense_iu *sense_iu;

    void *bounce;

    u32 bounce_size;
    u32 nbytes;

    u16 stream;

    bool data_in;
    bool data_expected;
    bool data_submitted;
    bool finishing;

    int pending;
    int error;
};

// Block MQ operations                                                     
extern const struct blk_mq_ops bot_mq_ops;
extern const struct blk_mq_ops uas_mq_ops;

// Utility functions                                                       
int ums_find_altsetting(struct usb_interface *intf, u8 proto);
void ums_copy_to_request(struct request *rq, const void *src);
void ums_copy_from_request(struct request *rq, void *dst);

bool ums_map_rq(struct ums_dev *dev,struct request *rq,u32 *lba,u32 *blocks,u32 *nbytes);
void ums_build_rw_cdb(u8 *cdb,bool write, u32 lba,u32 blocks);
void ums_dump_link_info(struct ums_dev *dev);

// BOT functions                                                          
int bot_find_endpoints(struct ums_dev *dev,struct usb_host_interface *alt);
void bot_reset_recovery(struct ums_dev *dev);

int bot_xfer_locked(struct ums_dev *dev,const u8 *cdb, u8 cdb_len, void *buf, u32 len, bool data_in);
int bot_sync_cmd(struct ums_dev *dev,const u8 *cdb,u8 cdb_len,void *buf,u32 len, bool data_in);
blk_status_t bot_queue_rq(struct blk_mq_hw_ctx *hctx,const struct blk_mq_queue_data *bd);

// UAS functions                                                           
int uas_find_endpoints(struct ums_dev *dev,struct usb_host_interface *alt);
int uas_alloc_streams(struct ums_dev *dev);
void uas_free_streams(struct ums_dev *dev);

int uas_enable(struct ums_dev *dev, int uas_alt);

int uas_sync_cmd(struct ums_dev *dev,const u8 *cdb,u8 cdb_len,void *buf,u32 len, bool data_in);
void uas_note_error(struct ums_dev *dev, int err);
void uas_restore(struct ums_dev *dev);

// SCSI functions                                                          
int ums_sync_cmd(struct ums_dev *dev,const u8 *cdb,u8 cdb_len, void *buf, u32 len,bool data_in);
int ums_inquiry(struct ums_dev *dev);
int ums_test_unit_ready(struct ums_dev *dev);
int ums_read_capacity(struct ums_dev *dev);

// Disk functions                                                          
int ums_bd_open(struct gendisk *disk, blk_mode_t mode);
void ums_bd_release(struct gendisk *disk);
extern const struct block_device_operations ums_bd_ops;

int ums_setup_block_device(struct ums_dev *dev);
void ums_remove_block_device(struct ums_dev *dev);

#endif

