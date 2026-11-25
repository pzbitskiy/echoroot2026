# From the SD card to a finished satellite on internal storage

The whole sequence: satellite on the card system, Home Assistant, speech
services, checks, install to the Echo's internal flash, and the bootloader
change that lets it start without a serial cable.

**Status:** done once, on one Echo gen1.
Steps marked "not tried" were not part of that run. The card on that unit
boots in 1-bit mode ([mmc/README.md](mmc/README.md)); nothing below depends on
that except the copy times.

Device names change with how the Echo was started:

| Started from | `mmcblk0` | `mmcblk1` |
|---|---|---|
| the card | the card (3 partitions) | internal flash (8 partitions) |
| internal flash, no card | internal flash | absent |

Internal flash partitions used here: 3 `boot` holds `u-boot.bin`, 6 `main-A`
holds the kernel and the Linux system.

Two things about the image's shell that the steps rely on:

- `scp` needs `-O` (the image has no `sftp-server`). It cannot replace a
  program that is running ("Text file busy" or a silent failure): stop the
  satellite first.
- `md5sum`, `find`, `diff`, `sort` exist only inside busybox: write
  `busybox md5sum ...`. `head`, `tail`, `cut` do not exist.

## 1. Back up the internal flash

Started from the card, with WiFi up ([page/SETUP.md](page/SETUP.md)). On the
PC; the file is about 4 GB and contains the device's stored credentials, so
keep it out of git:

```sh
ssh root@<echo> dd if=/dev/mmcblk1 bs=8M | dd of=EchoBackup.dd bs=8M
md5sum EchoBackup.dd
```

On the Echo, a few minutes:

```sh
busybox md5sum /dev/mmcblk1
```

Both sums must be equal. This backup is the only way back to Amazon's system.
Restoring it has not been tried.

## 2. Satellite on the card system

An image ([BUILD.md](BUILD.md)) already contains the
satellite and starts it at boot. Only the address has to be set, on the Echo:

```sh
vi /etc/echo-satellite.conf       # set:  allow = <address of Home Assistant>
/etc/init.d/S60satellite restart
grep echo-satellite /var/log/messages
```

Until `allow` is set, any host on the network can connect to the satellite
and open the microphone.

A card made from an older image needs the files first (this is what was done
in the run described here). On the PC:

```sh
docker/build.sh make -C satellite
scp -O satellite/echo-satellite      root@<echo>:/usr/bin/
scp -O satellite/S60satellite        root@<echo>:/etc/init.d/
scp -O satellite/echo-satellite.conf root@<echo>:/etc/
```

and on the Echo `chmod +x /etc/init.d/S60satellite`, then the three commands
above with `start` in place of `restart`.

Expected in the log: `api: listening on port 6053 as "echo"` and
`mic: opened /BMicsOut.shm, gain 20 dB`. Saying the wake word now lights the
ring and ends in the error pattern, with `request failed: Home Assistant is
not connected` in the log. That is correct until step 3.

For debugging, stop the service and run `echo-satellite -v` in the terminal.
`asrd` then has to be kept running by hand, which the init script otherwise
does.

## 3. Home Assistant

### Add the Echo

Settings → Devices & services → Add integration → **ESPHome**: host = the
Echo's address, port 6053, no encryption key.

Home Assistant then plays a short sound on the Echo and starts its voice
wizard ("Say 'Alexa' again"). If no speech services exist yet, the wizard
offers to install its own Whisper and Piper apps on the Home Assistant
machine. That works and is the quickest start; the Whisper app on defaults
runs Parakeet for English ([ha/README.md](ha/README.md)). Skip that offer if
the services are to run elsewhere.

### Speech-to-text on another machine

Server side: [ha/README.md](ha/README.md), "Install" (Docker) or "Without
Docker" (Python package and a systemd unit; this is what ran here, in an LXC
container). It listens on port 10300 and is itself the Wyoming service.

Home Assistant side:

1. Settings → Devices & services → Add integration → **Wyoming Protocol**:
   host = that machine, port 10300. The dialog asks for nothing else.
2. Settings → Voice assistants → the assistant → Speech-to-text: pick the new
   entry.
3. On the Echo's device page, check that this assistant is selected.

The new entry is called `faster-whisper`, the same as Home Assistant's own
app, because the name comes from the server and not from the integration
entry's title. To tell them apart rename the entity: Settings → Devices &
services → Entities, search `faster_whisper`, open the one that belongs to
the new integration entry, change its name. (From Home Assistant's source;
not confirmed on a running system.)

Text-to-speech stayed with Home Assistant's Piper app in this run. Moving it
is the same procedure with port 10200.

### Testing

1. **The server alone**, from any machine with Python 3 (see
   [ha/README.md](ha/README.md), "Measuring on your own machine"):

   ```sh
   python3 ha/stt-bench.py stt=<machine>:10300 -- ha/clips/*.wav
   ```

   All eight transcripts should be right. Here: 0.18 s mean after the end of
   the audio, Qwen3-ASR with `--vad-endpointing 0.5`.

2. **A live command.** Say "Alexa, what time is it", then a timer and a
   device command. The log (`grep echo-satellite /var/log/messages`, or the
   terminal with `-v`) shows what was heard and the times:

   ```
   wake word "Alexa" at sample ...
   timing: end of speech 2519 ms after the trigger
   heard: "Turn off the kitchen fan."
   response: "Turned off the fan"
   timing: first response audio 2577 ms after the trigger
   ```

3. **What Home Assistant received**, when the text is wrong. In
   `configuration.yaml`:

   ```yaml
   assist_pipeline:
     debug_recording_dir: /share/assist_pipeline
   ```

   After a restart every command is saved as a WAV file under
   `/share/assist_pipeline/<assistant>/<id>/`. They can be fed to
   `stt-bench.py` to compare engines on real audio. Remove the setting
   afterwards; these are recordings of the room.

Tried on the device: time, timers, device commands, the ring states, the
error pattern. Not tried on the device: buttons, the volume ring, changing
the wake word from Home Assistant, announcements.

## 4. Start at boot, still from the card

```sh
reboot        # see "Known problems" if it does not come back
```

After the restart, without logging in, a voice command must work. Then:

```sh
grep echo-satellite /var/log/messages
```

must show the two lines from step 2 with a time a few seconds after boot.

## 5. Put the setup on partition 3

`install.sh` copies the card's third partition to the internal flash, so
whatever the installed system should contain has to be there first. With a
current image the first two `cp` lines are not needed, the third still is
(it carries `allow`):

```sh
mkdir -p /tmp/p3 && mount /dev/mmcblk0p3 /tmp/p3
cp -a /usr/bin/echo-satellite      /tmp/p3/usr/bin/
cp -a /etc/init.d/S60satellite     /tmp/p3/etc/init.d/
cp -a /etc/echo-satellite.conf     /tmp/p3/etc/
cp -a /etc/wpa_supplicant.conf     /tmp/p3/etc/
cp -a /etc/network/interfaces      /tmp/p3/etc/network/
cp -a /etc/ssh/ssh_host_*          /tmp/p3/etc/ssh/
busybox md5sum /tmp/p3/boot/uImage
umount /tmp/p3 && sync
```

- The sum must be `9835684847e3e50b395534f06705989d`, the kernel the image
  was built with. On a card set up for 1-bit mode the running system's
  `/boot/uImage` is the patched card kernel; it has the same size and must
  not go onto partition 3.
- The ssh host keys are optional; with them the installed system keeps its
  ssh identity.
- The partition must be unmounted before the next step.

## 6. Install

This overwrites the bootloader file and the `main-A` system on the internal
flash. The other six partitions are not written.

```sh
cd /home/root && ./install.sh
```

It copies 1000 MB. At 1 to 2 MB/s from a 1-bit card allow 10 to 20 minutes;
do not interrupt it.

### Check the copy

The two partitions are not byte-identical afterwards: `install.sh` deletes
itself from the copy. Compare file by file instead, both sides read-only:

```sh
mkdir -p /tmp/src /tmp/dst
mount -o ro /dev/mmcblk0p3 /tmp/src
mount -o ro /dev/mmcblk1p6 /tmp/dst
(cd /tmp/src && busybox find . -type f -exec busybox md5sum {} + > /tmp/src.md5)
(cd /tmp/dst && busybox find . -type f -exec busybox md5sum {} + > /tmp/dst.md5)
busybox diff /tmp/src.md5 /tmp/dst.md5
umount /tmp/src /tmp/dst
```

The only changed line must be the removal of `./home/root/install.sh`.

The bootloader file:

```sh
mount -o ro /dev/mmcblk1p3 /tmp/mnt.boot
busybox md5sum /usr/share/MISTO_INT/u-boot.bin /tmp/mnt.boot/u-boot.bin
umount /tmp/mnt.boot
```

Both sums must be equal.

## 7. First start from internal flash

Keep the serial cable in the PC for this start (the reason is step 8). Power
off, take the card out, power on. Then over ssh:

```sh
cat /proc/cmdline                         # contains root=/dev/mmcblk0p6
grep echo-satellite /var/log/messages     # the two lines from step 2
```

On the console, U-Boot prints `booting ...main-B` or `main-A`. That name
comes from Amazon's boot-slot data and is only printed; this bootloader
always loads the kernel and system from partition 6.

Keep the card unchanged until this has worked: it is the way back in.

## 8. Bootloader that starts without a serial cable

### The problem

The bootloader in the image (`MISTO_INT`) waits 2 seconds for a key on the
serial console, and any received character stops it at `uboot >` for good.
The receive pin has no pull resistor configured. With the adapter unplugged
from the PC, or with nothing connected, the line is not held idle and is read
as a key press: the Echo then never starts Linux. Amazon's bootloader has
`bootdelay=0` and no key check.

To confirm on a unit: power on with the cable out of the PC, wait a minute,
plug the cable into the PC and press Enter. A `uboot >` prompt is this
problem; `boot` continues from there.

### Build

The same bootloader with the wait set to 0, which also removes the key check.
The setting is overridden in the build folder; the source is unchanged.

```sh
docker/build.sh sh -c 'make -C u-boot O=MISTO_INT0 misto_int_defconfig &&
  printf "#undef CONFIG_BOOTDELAY\n#define CONFIG_BOOTDELAY 0\n" \
      >> u-boot/MISTO_INT0/include/config.h &&
  make -C u-boot O=MISTO_INT0 -j8'
strings u-boot/MISTO_INT0/u-boot.bin | grep '^bootdelay='     # bootdelay=0
md5sum u-boot/MISTO_INT0/u-boot.bin
```

The checksum differs from build to build because the build time is in the
file.

### Install on a device that is already set up

On the PC:

```sh
scp -O u-boot/MISTO_INT0/u-boot.bin root@<echo>:/tmp/
```

On the Echo, **started from internal flash without the card**:

```sh
busybox md5sum /tmp/u-boot.bin          # the sum from the PC
mkdir -p /tmp/boot && mount /dev/mmcblk0p3 /tmp/boot
ls /tmp/boot                            # u-boot.bin, not a Linux tree
cp /tmp/u-boot.bin /tmp/boot/u-boot.bin
sync
busybox md5sum /tmp/boot/u-boot.bin     # the same sum
umount /tmp/boot
```

Power off, unplug the cable from the PC, power on: the Echo must come up on
its own. It did here.

- With this bootloader the start from internal flash can no longer be
  stopped from the console.
- Way back: start from the card with the cable in the PC, mount
  `/dev/mmcblk1p3` and copy `/usr/share/MISTO_INT/u-boot.bin` onto it.
- `install.sh` installs the file `/usr/share/MISTO_INT/u-boot.bin` of the
  running card system. Running it again puts the 2-second bootloader back,
  unless that file is replaced with the new one first.
- The card's own bootloader keeps its 2-second wait. Starting from the card
  therefore needs the serial adapter plugged into a PC.

### Fix it in the source, for new images

The repository still builds the 2-second bootloader. To make every new image
install one without the wait, change one line in the `u-boot` submodule,
`u-boot/include/configs/misto_int_defconfig.h`:

```diff
 #undef CONFIG_BOOTDELAY
-#define CONFIG_BOOTDELAY 2
+#define CONFIG_BOOTDELAY 0
```

and rebuild. The old build folder has to go, because the build does not
notice a changed configuration header:

```sh
rm -rf u-boot/MISTO_INT
docker/build.sh
strings u-boot/MISTO_INT/u-boot.bin | grep '^bootdelay='      # bootdelay=0
```

`sdcard.img` then carries the new file as `/usr/share/MISTO_INT/u-boot.bin`,
`install.sh` installs it in step 6, and the rest of this step is not needed.
To fix a card that is already written, copy the file to that path on the
running card system before step 6.

- Checked: with this one line changed, the bootloader comes out identical to
  the `MISTO_INT0` build that runs on the device, apart from the build time.
  The full sequence (new image, `install.sh`, start without cable) was not
  run.
- The card's bootloader has its own setting in `misto_sd_defconfig.h` and is
  not affected. Leave that one at 2: on the card the wait is the way to the
  `uboot >` prompt, and a card is only used with the serial cable attached.
- The change is in a submodule. Commit it there, or keep it as a patch the
  way `mmc/u-boot-sd-1bit.patch` is kept.

## Afterwards

- **Updates** go over ssh: `scp -O` the new file, after stopping the service
  when it is the satellite binary.
- **System log.** It is written to the flash, at most 200 KB plus one older
  copy. The satellite adds 12 lines per voice command and nothing while
  idle; the audio daemon adds one line every 33 s. To keep the log in RAM
  instead (lost at power-off; tried under emulation only):

  ```sh
  df /tmp        # must show tmpfs
  echo 'SYSLOGD_ARGS="-n -O /tmp/messages"' > /etc/default/logging
  /etc/init.d/S01logging restart
  ```

- **Removing the card wiring** removes the way back in: a bootloader, kernel
  or filesystem fault on the flash, and restoring the backup, all need a
  start from the card. Keep the card as it is and the wires on the module.

## Known problems

- `reboot` on the card system did not come back and needed a power cycle.
  Probably step 8's problem in the card's bootloader, if the cable was not in
  the PC; not established. Not tried on the installed system.
- With the cable disconnected the same undriven pin feeds the Linux login
  prompt on the serial port. Not checked whether that leaves anything in the
  log.
- A rebuilt `sdcard.img` is generic: no WiFi settings, no `allow`, and the
  2-second bootloader unless the source fix from step 8 was made.
