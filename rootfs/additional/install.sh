if [ ! -b /dev/mmcblk0p3 ]; then
    echo "source partition does not exist."
    exit 1
fi
if [ ! -b /dev/mmcblk1p2 ]; then
    echo "recovery partition does not exist."
    exit 1
fi
if [ ! -b /dev/mmcblk1p3 ]; then
    echo "boot partition does not exist."
    exit 1
fi
if [ ! -b /dev/mmcblk1p6 ]; then
    echo "target partition does not exist."
    exit 1
fi
if [ ! -f /usr/share/MISTO_INT/u-boot.bin ]; then
    echo "new u-boot does not exist."
    exit 1
fi
cd /
umount /dev/mmcblk1p? 2> /dev/null
umount /dev/mmcblk0p3 2> /dev/null
mkdir -p /tmp/mnt.recovery /tmp/mnt.boot /tmp/mnt.target
mount /dev/mmcblk1p2 /tmp/mnt.recovery
mount /dev/mmcblk1p3 /tmp/mnt.boot
cp -a /usr/share/MISTO_INT/u-boot.bin /tmp/mnt.boot
umount /dev/mmcblk1p? 2> /dev/null
sync
dd if=/dev/mmcblk0p3 of=/dev/mmcblk1p6 bs=8M
sync
mount /dev/mmcblk1p6 /tmp/mnt.target
rm /tmp/mnt.target/home/root/install.sh
umount /tmp/mnt.target
sync
