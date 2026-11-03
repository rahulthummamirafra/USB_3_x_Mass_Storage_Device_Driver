obj-m += usb_ms_driver.o

usb_ms_driver-y := ums_main.o \
                  ums_bot.o \
                  ums_disk.o \
                  ums_scsi.o \
                  ums_uas.o \
                  ums_utils.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean


