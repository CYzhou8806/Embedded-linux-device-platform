obj-m += custom_acq.o
# custom_acq_trace.h is included by <trace/define_trace.h> with
# TRACE_INCLUDE_PATH ".", which resolves against the include path, not the
# including file's directory.
CFLAGS_custom_acq.o := -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
