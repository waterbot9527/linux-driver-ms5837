# Makefile for BQ40Z50 driver

# 如果定义了KERNELRELEASE表示我们被内核构建系统调用
ifneq ($(KERNELRELEASE),)
	obj-m := ms5837.o
	ms5837-objs := ms5837.o

# 否则我们是直接从命令行调用的
else
	KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install
	depmod -a

endif
