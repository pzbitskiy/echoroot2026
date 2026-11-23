# Booting from an SD card in 1-bit mode

For an Echo whose hand-soldered SD wiring cannot carry 4-bit transfers or a
48 MHz clock. The stock boot chain then fails in U-Boot or in the kernel. With
the two changes in this folder the Echo boots from the card using one data
line at about 20 MHz, the way X-Loader reads the card anyway.

Worked out on one Echo (board revision 0x1a, EVT-3.2) with a
32 GB SDHC card in a 9-pin microSD breakout module, 12 cm wires and 3.3 V from
TP19. Everything below was observed on that unit.

| File | What it is |
|---|---|
| `u-boot-sd-1bit.patch` | Patch for the `u-boot` submodule: new config `misto_sd1bit_defconfig` that keeps the SD slot in 1-bit mode at default speed |
| `slow-sd-kernel.py` | Makes patched copies of the stock kernel `uImage` that drive the SD slot slower |

## Do you have this problem?

X-Loader loads U-Boot from the card, then one of these happens.

Stock U-Boot from the card (`MISTO_SD`):

```
Booting from mmc ...
** No partition table - mmc 0 **
Wrong Image Format for bootm command
ERROR: can't get kernel image!
resetting ...
```

sometimes with `mmc_send_cmd: timedout waiting on cmd inhibit to clear` a few
lines earlier. The board then resets and loops.

Stock kernel, once U-Boot manages to load it:

```
mmc0: error -110 whilst initialising SD card
...
No filesystem could mount root, tried:  ext3
Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(179,2)
```

The panic is a side effect: without the card, the internal eMMC becomes
`mmcblk0`, and its partition 2 is not a root filesystem.

Check the simple causes first:

- The card is written from the whole image: `sudo fdisk -l /dev/sdX` shows
  three partitions starting at sectors 1, 32769 and 2080769.
- The card has power. With the 3.3 V wire off, the Echo boots its stock
  firmware from eMMC, which looks like success. See "Which system booted?"
  below.

## Boot in 1-bit mode

### 1. Build the bootloader

The change lives in the `u-boot` submodule's working tree. In a fresh checkout,
apply it first:

```sh
git -C u-boot apply ../mmc/u-boot-sd-1bit.patch
```

Build (needs the build container, see `BUILD.md`):

```sh
docker/build.sh sh -c 'make -C u-boot O=MISTO_SD1BIT misto_sd1bit_defconfig && make -C u-boot O=MISTO_SD1BIT -j8'
```

Result: `u-boot/MISTO_SD1BIT/u-boot.bin`. The normal `MISTO_SD` and `MISTO_INT`
builds are not affected; the change is behind `CONFIG_LAB126_SD_1BIT`.

### 2. Make the kernel

```sh
python3 mmc/slow-sd-kernel.py rootfs/out/boot/uImage sdcard/images
```

Result: `sdcard/images/uImage-nohs` and `sdcard/images/uImage-1bit`. The
script only accepts the kernel build it was written for (Linux 2.6.37,
`#1 Fri Oct 2 04:03:28 UTC 2020`, 2599640 bytes) and stops on any other.

### 3. Put both on the card

With the card in a reader on the PC (`sdX` is the card):

```sh
sudo mount /dev/sdX1 /mnt
sudo cp u-boot/MISTO_SD1BIT/u-boot.bin /mnt/u-boot.bin
sudo umount /mnt

sudo mount /dev/sdX2 /mnt
sudo cp /mnt/boot/uImage /mnt/boot/uImage-stock
sudo cp sdcard/images/uImage-1bit /mnt/boot/uImage
sudo cp sdcard/images/uImage-1bit sdcard/images/uImage-nohs /mnt/boot/
sync
sudo umount /mnt
```

Leave partition 3 alone. It is the image `install.sh` copies to the eMMC, and
the installed system must keep the stock kernel.

### 4. Boot

Power on. A boot from the card in 1-bit mode shows:

```
U-Boot 2010.06-00011-ge1ab9145d5-dirty (...)
...
Booting from mmc ...
2599640 bytes read in 2298 ms (1.1 MiB/s)
   Verifying Checksum ... OK
...
mmc0: new SDHC card at address 0007
mmcblk0: mmc0:0007 SD32G 28.9 GiB
 mmcblk0: p1 p2 p3
```

To try another kernel without changing the card, press a key during the
2-second countdown after `booting ...main-B` and type:

```
setenv kernel_file /boot/uImage-nohs
run mmcboot
```

### Which system booted?

| | From the card | Stock firmware from eMMC (this unit) |
|---|---|---|
| First X-Loader banner | `Jun 29 2016 - 17:23:55` (the `MLO` in `x-load/`) | the eMMC's own X-Loader, a different date |
| Kernel size read by U-Boot | 2599640 bytes | 2605656 bytes |
| `DSPLINK Module ... created on Date:` | `Oct  2 2020` | `Jul  8 2018` |
| `/proc/partitions` | `mmcblk0` is the card (three partitions), `mmcblk1` the eMMC | `mmcblk0` is the eMMC (eight partitions) |

### Consequences

- The card is slow: 1 to 2 MB/s. `install.sh` copies 1000 MB from it, so
  allow 10 to 20 minutes, and compare afterwards file by file as described in
  [INSTALL.md](../INSTALL.md), step 6. A checksum over the two partitions
  never matches, because `install.sh` deletes itself from the copy.
- The card's bootloader waits 2 seconds for a key on the serial console and
  takes an undriven receive line for one. Starting from the card therefore
  needs the serial adapter plugged into a PC (INSTALL.md, step 8).
- Both patched kernels also remove high speed from the WiFi chip's SDIO link
  (24 MHz instead of 48 MHz). The eMMC is not affected.
- The patched kernel is only for booting from the card. The system installed
  on the eMMC runs the stock kernel at full speed.
- A rebuilt `sdcard.img` contains the stock bootloader and kernel. After
  writing it to the card, repeat step 3. To update only the image that gets
  installed, write the new `rootfs.ext3` to `/dev/sdX3` and leave the other
  partitions as they are.

## What was wrong

Three stages read the card, each in its own way:

| Stage | Data lines | Clock | Result on this wiring |
|---|---|---|---|
| X-Loader (`x-load/cpu/omap3/mmc.c`) | 1 (never leaves 1-bit mode) | default speed, taken from the card's CSD | always worked |
| U-Boot `MISTO_SD` | 4 | 48 MHz (high speed) | failed |
| Stock kernel | 4 | 48 MHz (high speed) | failed |
| 1-bit U-Boot, then `uImage-1bit` | 1 | 19.2 MHz, then 24 MHz | works |

Two separate faults showed up.

**At 48 MHz even commands fail.** In U-Boot the first read after card setup
fails at the command stage. In the kernel, card setup stops with `-110`
(timeout) right after the line `host does not support reading read-only
switch`, which is where it raises the clock and switches the bus width.

**In 4-bit mode the card loses its place when several data lines fall
together.** With the clock slowed to 400 kHz or 6 MHz, U-Boot reads went
through and the data showed the pattern: every time the bus went from `1111`
to `0000`, one half-byte was missing. Sector 0 of the image ends like this:

```
expected  ... 00 40 1f 00 | 00 85 06 81 83 01 41 01 01 c0 1f 00 00 40 1f 00 | 00 ... 00 55 aa
read      ... 04 01 f0 00   85 06 81 83 01 41 01 01 c0 1f 00 04 01 f0 00 ... 00 55 aa 51 f8
```

The `0` after each `f` is gone, `55 aa` arrives two bytes early, and the two
bytes after it (`51 f8`) are the start of the card's CRC (`51 f0 83 ...`) with
the same loss. The lost step does not depend on the clock rate.

The likely mechanism, inferred from the pattern and not measured: when the
card pulls several lines low at once, the current spike through the ground
wire lifts the card's ground for a moment, or the data wires couple into the
clock wire. The card takes that as an extra clock edge and moves on one step;
the Echo, sampling on its real clock, never sees the skipped half-byte. In
1-bit mode only one line switches, which is not enough to trigger it.

The kernel showed the same fault from the other side. With high speed removed
(`uImage-nohs`, 4-bit at 24 MHz) the card was detected, and then every read
failed:

```
mmcblk0: error -110 sending read/write command, response 0x0, card status 0x400b00
mmcblk0: error -110 sending read/write command, response 0x0, card status 0x800b00
```

Status `...b00` is the card's "sending data" state. `0x800000` is "the last
command had a bad CRC", `0x400000` is "illegal command". So the card accepted
the first read, started streaming on four lines, and from then on the commands
reaching it were corrupted.

### How the wiring changed the pattern

| Wiring | Half-bytes lost when |
|---|---|
| All wires 30 cm | all four lines fall together |
| Data wires 12 cm, ground still 30 cm with joins | two or more lines fall together (worse) |
| Card ground run to the UART adapter instead of the Echo | all four fall together |
| Short direct wire, Echo GND pad to module pin 2 | not measured in U-Boot; the kernel still fails in 4-bit mode |

Shortening the data wires while the ground stayed long made it worse. The
ground return matters most: it should be the shortest wire, go straight from
the Echo's GND pad to the card, and carry nothing else. The UART ground can be
attached at the card end. On this unit the Echo's GND pad was hard to solder
(it is tied to the ground plane and needs a larger tip and more heat than the
signal pads).

Not tried: 47 to 100 pF from CLK to GND at the card, series resistors of 22 to
100 ohm in D0 to D3 at the card end, a different (older, slower) card, wires
shorter than 12 cm.

## The patches

### U-Boot

`board_mmc_init()` in `board/ti/lab126/evm.c` registers the SD slot with
4-bit and both high-speed modes masked out when `CONFIG_LAB126_SD_1BIT` is
defined:

```c
omap_mmc_init(0, MMC_MODE_4BIT | MMC_MODE_HS | MMC_MODE_HS_52MHz, 0, -1, -1);
```

The MMC core then leaves the card in 1-bit mode at default speed, and the
driver limits the clock to 20 MHz (19.2 MHz with its divider). The eMMC slot
is unchanged. `misto_sd1bit_defconfig` is `misto_sd_defconfig` plus that
define, so the boot environment is the same (`mmc_part=0:2`,
`root=/dev/mmcblk0p2`, 2-second boot delay).

### Kernel

The kernel is Amazon's prebuilt `uImage`, a zImage with a gzip payload.
`slow-sd-kernel.py` unpacks it, changes a few bytes, and packs it again to
exactly the same size, so everything outside the payload stays in place.
Addresses are virtual addresses in the unpacked kernel (base `0xc0008000`).

| Address | Change | Effect | In |
|---|---|---|---|
| `0xc037aba8` | `orr r3, r3, #6` becomes `orr r3, r3, #2` in `omap_hsmmc_probe` | `MMC_CAP_SD_HIGHSPEED` is no longer set: SD cards and SDIO stay at default speed, 24 MHz | both |
| `0xc04e3754` | SD slot `caps` 1 becomes 0 in the board's two-controller slot table | no `MMC_CAP_4_BIT_DATA` for the SD slot | `uImage-1bit` |
| `0xc04e37d8` | the same in the three-controller table (SD, eMMC, WiFi) | as above | `uImage-1bit` |

Repacking details:

- The new gzip stream must not be longer than the old one. A shorter one is
  padded with a file name in the gzip header; the kernel's decompressor skips
  it (checked in the disassembly of the zImage head).
- The uImage data and header CRCs are recalculated.
- The script verifies the result by unpacking it again and comparing with the
  patched kernel.

Tested on hardware: `uImage-nohs` detects the card and then fails on 4-bit
reads, as described above; `uImage-1bit` boots the system from the card.

## Diagnosing from the U-Boot prompt

Press a key during the 2-second countdown after `booting ...main-B` to get the
`uboot >` prompt. This applies to the U-Boot builds from this repository.

```
mmcinfo                      # card details, bus width, speed
mmc read 0x82000000 0 2      # sectors 0 and 1 into RAM
md.b 0x820001b0 0x50         # partition table; must end "55 aa" at 0x820001fe
md.b 0x82000200 0x10         # must start "eb 3c 90 6d 6b 66 73 2e" (".<.mkfs.")
```

Expected partition table for the image built from `sdcard/genimage.cfg`:

```
820001b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 00
820001c0: 02 00 0c 0a 09 02 01 00 00 00 00 80 00 00 00 0a
820001d0: 0a 02 83 85 05 81 01 80 00 00 00 40 1f 00 00 85
820001e0: 06 81 83 01 41 01 01 c0 1f 00 00 40 1f 00 00 00
820001f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 55 aa
```

Two things about this U-Boot's MMC driver (`drivers/mmc/omap_hsmmc.c`) make
its messages misleading:

- **`blocks read: OK` does not mean the data is good.** `mmc_send_cmd()`
  ignores the return value of `mmc_read_data()`, so CRC errors in the data
  phase are not reported. Look at the bytes. Loading a kernel is a real test,
  because `bootm` verifies the uImage checksum.
- **One failed read blocks all later ones.** After a read command times out,
  only the command half of the controller is reset. The data half stays busy,
  and every later command prints `timedout waiting on cmd inhibit to clear`.
  This is why the stock build shows that line during boot, and why reads typed
  at the prompt afterwards fail without touching the card.

Slowing the SD clock in the stock build, to separate speed problems from
4-bit problems. `0x4809c12c` is the SYSCTL register of the SD controller
(MMC1); `000e0087` means divisor 2 of 96 MHz, clock on:

```
mmcinfo
md.l 0x4809c12c 1            # expect 000e0087
mw.l 0x4809c12c 0x000e0083   # card clock off
mw.l 0x4809c12c 0x000e0403   # divisor 16: 6 MHz   (0x000e3c03: divisor 240, 400 kHz)
mw.l 0x4809c12c 0x000e0407   # clock on            (0x000e3c07)
mmc read 0x82000000 0 2
```

This only works while the controller is not blocked by an earlier failed
read. Writing `0x060e0087` to the same register resets both halves of the
controller; on the last wiring that was not enough to get a read through, and
the 1-bit build replaced these tests.

The writes change only the SD controller's clock and are undone by a reset.
A board left in the reset loop rewrites a small settings file on the eMMC on
every pass, so do not leave it looping for long.
