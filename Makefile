# Out-of-tree override of drivers/gpu/drm/tiny/ili9486.c.
# Build:   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
# Install: sudo install -D -m 644 ili9486.ko /lib/modules/$(uname -r)/updates/ili9486.ko && sudo depmod -a
obj-m += ili9486.o
