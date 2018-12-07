obj-m += vircon.o 

KERNELVER ?= $(shell uname -r)
KERNELDIR ?= /lib/modules/$(KERNELVER)
PWD  := $(shell pwd)

all: default fbvncserver

default:
	$(MAKE) -C $(KERNELDIR)/build M=$(PWD) modules
fbvncserver:
	$(CC) -o fbvncserver fbvncserver.c -l vncserver
install: all
	cp vircon.ko  $(KERNELDIR)/kernel/drivers/video
	cp fbvncserver /usr/local/bin
	depmod -a $(KERNELVER) /lib/modules
clean:
	@rm -fr *.ko *.o rm *.symvers *.order *.mod.c .vircon* .tmp_versions fbvncserver
