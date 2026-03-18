KERNELDIR := /home/xy/linux/alientek_linux/linux-imx-4.1.15-2.1.0-e48931b1-v2.8
CURRENT_PATH := $(shell pwd)
obj-m := HX711.o L298n.o tcrt5000.o

build: kernel_modules

kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean