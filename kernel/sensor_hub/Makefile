ifneq ($(KERNELRELEASE),)

obj-m := sensor_hub.o
sensor_hub-y := sensor_hub_core.o sensor_hub_pir.o sensor_hub_sht20.o
sensor_hub-y += sensor_hub_buzzer.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
