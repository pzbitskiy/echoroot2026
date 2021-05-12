all: sdcard/images/sdcard.img
.PHONY: clean

clean:
	$(MAKE) -C rootfs clean
	$(MAKE) -C u-boot clean
	$(MAKE) -C sdcard clean

rootfs/out/.stamp_done: rootfs/buildroot/Makefile
	$(MAKE) -C rootfs

u-boot/include/config.mk: u-boot/Makefile
	make -C u-boot misto_defconfig

u-boot/u-boot.bin: u-boot/include/config.mk
	$(MAKE) -C u-boot

sdcard/images/sdcard.img: u-boot/u-boot.bin rootfs/out/.stamp_done
	make -C sdcard
