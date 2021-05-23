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

voice/porcupine_demo_shm: voice/porcupine_demo_shm.c voice/Makefile
	$(MAKE) -C voice porcupine_demo_shm

voice/porcupine_send_shm: voice/porcupine_send_shm.c voice/Makefile
	$(MAKE) -C voice porcupine_send_shm

rootfs/out/.stamp_post: u-boot/MISTO_INT/u-boot.bin voice/porcupine_demo_shm voice/porcupine_send_shm rootfs/out/.stamp_done
	@echo "Doing some post-modifications in rootfs"
	@rm -f rootfs/out/.stamp_done
	@mkdir -p rootfs/out/usr/share/MISTO_INT
	@cp -f u-boot/MISTO_INT/u-boot.bin rootfs/out/usr/share/MISTO_INT/
	@mkdir -p rootfs/out/usr/share/porcupine
	@cp -f voice/porcupine/lib/beaglebone/libpv_porcupine.so rootfs/out/usr/lib/
	@echo "Patching the libpv_porcupine.so for non-hardfloat loading"
	@printf '\x02' | dd of=rootfs/out/usr/lib/libpv_porcupine.so bs=1 seek=37 count=1 conv=notrunc
	@echo "Installing porcupine files"
	cp -f voice/porcupine/lib/common/porcupine_params* rootfs/out/usr/share/porcupine/
	cp -rf voice/porcupine/resources/keyword_files* rootfs/out/usr/share/porcupine/
	cp -f voice/porcupine_demo_shm rootfs/out/usr/bin/
	cp -f voice/porcupine_send_shm rootfs/out/usr/bin/
	touch rootfs/out/.stamp_post

sdcard/images/sdcard.img: u-boot/MISTO_SD/u-boot.bin rootfs/out/.stamp_post
	@rm -f rootfs/out/.stamp_post
	make -C sdcard
