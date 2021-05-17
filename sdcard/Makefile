all: images/sdcard.img
.PHONY: clean

clean:
	rm -rf images/

images/rootfs.ext3:
	@mkdir -p images
	fakeroot -- genext2fs -z -b 1024000 -N 24000 -d "../rootfs/out" "$@"
	tune2fs -U random "$@"
	tune2fs -j -J size=1 -O "filetype,sparse_super" -L "echoroot" "$@"
	e2fsck -pDf "$@" || true
	tune2fs -c 0 -i 0 "$@"

images/MLO:
	@mkdir -p images
	cp ../x-load/MLO $@

images/u-boot.bin:
	@mkdir -p images
	cp ../u-boot/MISTO_SD/u-boot.bin $@


images/sdcard.img: images/rootfs.ext3 images/MLO images/u-boot.bin
	rm -rf genimage.tmp
	../rootfs/br2echo/host/usr/bin/genimage \
	--rootpath "images" \
	--tmppath "genimage.tmp" \
	--inputpath "images" \
	--outputpath "images" \
	--config "genimage.cfg"
	rm -rf genimage.tmp
