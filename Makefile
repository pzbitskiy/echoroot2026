all: sdcard/images/sdcard.img
.PHONY: clean

clean:
	$(MAKE) -C rootfs clean
	rm -rf u-boot/MISTO_INT
	rm -rf u-boot/MISTO_SD
	$(MAKE) -C sdcard clean

rootfs/out/.stamp_done: rootfs/buildroot/Makefile
	$(MAKE) -C rootfs

u-boot/MISTO_INT/include/config.mk: u-boot/Makefile
	make -C u-boot O=MISTO_INT misto_int_defconfig

u-boot/MISTO_INT/u-boot.bin: u-boot/MISTO_INT/include/config.mk
	$(MAKE) -C u-boot O=MISTO_INT

u-boot/MISTO_SD/include/config.mk: u-boot/Makefile
	make -C u-boot O=MISTO_SD misto_sd_defconfig

u-boot/MISTO_SD/u-boot.bin: u-boot/MISTO_SD/include/config.mk
	$(MAKE) -C u-boot O=MISTO_SD

sdcard/images/sdcard.img: u-boot/MISTO_INT/u-boot.bin u-boot/MISTO_SD/u-boot.bin rootfs/out/.stamp_done
	@rm -f rootfs/out/.stamp_done
	@mkdir -p rootfs/out/usr/share/MISTO_INT
	@cp -r u-boot/MISTO_INT/u-boot.bin rootfs/out/usr/share/MISTO_INT
	make -C sdcard
