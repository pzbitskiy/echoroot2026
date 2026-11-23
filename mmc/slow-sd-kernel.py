#!/usr/bin/env python3
"""Make copies of the Echo kernel (uImage) that drive the SD slot gently.

For SD wiring that cannot take 48 MHz or four data lines switching at once.
The stock kernel switches every SD card to high speed (48 MHz) and 4-bit
mode; with long hand-soldered wires it then fails with
"mmc0: error -110 whilst initialising SD card".

  uImage-nohs   no SD high speed: SD card and WiFi (SDIO) run at 24 MHz,
                the SD slot stays 4-bit. The eMMC is not affected.
  uImage-1bit   as above, and the SD slot uses one data line.

Usage: slow-sd-kernel.py <stock uImage> <output directory>

Only for the copy of the kernel on the SD card's root partition. The patches
are byte changes in the unpacked kernel, checked against the expected
original bytes, so the script refuses any other kernel build.
"""
import gzip
import struct
import sys
import zlib

BASE = 0xC0008000  # virtual address of the unpacked kernel

# (virtual address, expected bytes, new bytes, what)
NO_HIGH_SPEED = [
    # omap_hsmmc_probe: mmc->caps |= MMC_HIGHSPEED | SD_HIGHSPEED | ... ;
    # "orr r3, r3, #6" -> "orr r3, r3, #2" drops MMC_CAP_SD_HIGHSPEED.
    (0xC037ABA8, bytes.fromhex("063083e3"), bytes.fromhex("023083e3"),
     "omap_hsmmc_probe: no MMC_CAP_SD_HIGHSPEED"),
]
ONE_BIT = [
    # Board tables of MMC slots, {mmc, caps, ...}, 44 bytes per slot. Slot
    # "mmc = 1" is the SD slot; caps 1 is MMC_CAP_4_BIT_DATA. There are two
    # tables (two and three controllers); patch the SD slot in both.
    (0xC04E3750, struct.pack("<II", 1, 1), struct.pack("<II", 1, 0),
     "board table A: SD slot caps 4-bit -> none"),
    (0xC04E37D4, struct.pack("<II", 1, 1), struct.pack("<II", 1, 0),
     "board table B: SD slot caps 4-bit -> none"),
]


def die(msg):
    sys.exit("slow-sd-kernel: " + msg)


def unpack(uimage):
    magic, _hcrc, _time, size, _load, _ep, dcrc = struct.unpack(">7I", uimage[:28])
    if magic != 0x27051956 or len(uimage) != 64 + size:
        die("not a uImage, or trailing data")
    zimage = uimage[64:]
    if zlib.crc32(zimage) != dcrc:
        die("uImage data checksum is wrong")
    start = zimage.find(b"\x1f\x8b\x08")
    while start >= 0:
        d = zlib.decompressobj(31)
        try:
            image = d.decompress(zimage[start:])
            if d.eof:
                break
        except zlib.error:
            pass
        start = zimage.find(b"\x1f\x8b\x08", start + 1)
    if start < 0:
        die("no gzip stream in the zImage")
    end = len(zimage) - len(d.unused_data)
    return zimage, start, end, image


def compress_to(image, length):
    """A gzip stream of exactly `length` bytes that inflates to `image`.

    The kernel's decompressor skips a file name in the gzip header, so a
    shorter stream is padded with one. The stream must not grow: the bytes
    after it and the size word at its end have fixed places in the zImage.
    """
    best = None
    for level in (9, 8, 7, 6):
        for mem in (9, 8):
            c = zlib.compressobj(level, zlib.DEFLATED, -15, mem)
            raw = c.compress(image) + c.flush()
            if best is None or len(raw) < len(best):
                best = raw
    trailer = struct.pack("<II", zlib.crc32(image), len(image))
    pad = length - (10 + len(best) + 8)
    if pad < 0:
        die("recompressed kernel is %d bytes too long" % -pad)
    if pad == 0:
        header = b"\x1f\x8b\x08\x00" + b"\0" * 4 + b"\x02\x03"
    else:
        header = b"\x1f\x8b\x08\x08" + b"\0" * 4 + b"\x02\x03" + b"x" * (pad - 1) + b"\0"
    out = header + best + trailer
    assert len(out) == length and gzip.decompress(out) == image
    return out


def build(uimage, patches):
    zimage, start, end, image = unpack(uimage)
    image = bytearray(image)
    for va, old, new, what in patches:
        off = va - BASE
        if bytes(image[off:off + len(old)]) != old:
            die("unexpected bytes at %#x (%s): different kernel build?" % (va, what))
        image[off:off + len(new)] = new
    image = bytes(image)
    zimage = zimage[:start] + compress_to(image, end - start) + zimage[end:]
    header = bytearray(uimage[:64])
    struct.pack_into(">I", header, 24, zlib.crc32(zimage))
    struct.pack_into(">I", header, 4, 0)
    struct.pack_into(">I", header, 4, zlib.crc32(bytes(header)))
    out = bytes(header) + zimage
    # Check the result the way U-Boot and the kernel will read it.
    _, _, _, check = unpack(out)
    assert check == image and len(out) == len(uimage)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    uimage = open(sys.argv[1], "rb").read()
    for name, patches in (("uImage-nohs", NO_HIGH_SPEED),
                          ("uImage-1bit", NO_HIGH_SPEED + ONE_BIT)):
        path = "%s/%s" % (sys.argv[2], name)
        with open(path, "wb") as f:
            f.write(build(uimage, patches))
        print(path)
        for va, _old, _new, what in patches:
            print("    %#x  %s" % (va, what))


if __name__ == "__main__":
    main()
