obj-m := backfire.o

all:	modules modules_install
	@echo Done

modules:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

modules_install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	@rm -f *.o Module.markers modules.order
