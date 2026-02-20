obj-m += gpio.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	@echo "Building GPIO kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	@echo "Cleaning build files..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean
