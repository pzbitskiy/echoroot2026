# The software part

What the image is, how to build and install it, and how to set up the parts
that need settings of your own. The complete sequence from a fresh card to a
finished Home Assistant voice satellite on internal storage, with every check
along the way, is in [INSTALL.md](../INSTALL.md); this page is the overview.

## What the image runs
The image is a hybrid: Amazon's kernel (2.6.37), DSP firmware and audio, LED
and button daemons, with a newer base system from buildroot on top and
without the Alexa services. At boot it starts

* `audioproxyd` and the DSP, which turn the seven microphones into one
  beamformed and echo-cancelled signal,
* `ledd`, `buttond` and `audioCtrl` for the LED ring, the buttons and the
  volume,
* PulseAudio for the speaker and `btmd-ng` for Bluetooth,
* `asrd`, Amazon's wake-word engine, and `echo-satellite`, which connects the
  Echo to Home Assistant as a voice satellite (see [Voice](VOICE.md)).

## Building
In case you want to build it yourself instead of using prebuilt images, the
build runs in a Docker container that brings the old tools it needs. You need
Docker, git, make, curl and about 15GB disk space:
```
git submodule update --init
make -C rootfs -j8 download
docker/build.sh make -C rootfs/br2echo olddefconfig
docker/build.sh
```
The result is `sdcard/images/sdcard.img`. [BUILD.md](../BUILD.md) explains
what the build does, what it depends on and how to add software to the image.

Building without the container was the original way and has not been tried
since. It needs a system old enough for buildroot 2017.05 and u-boot 2010.06
(current distributions are not; the container is Ubuntu 18.04, see "Build
environment" in BUILD.md), and on it:

* GNU Awk >= 4.2.1 or mawk >= 1.3.3
* binutils >= 2.34
* An arm-linux-gnueabihf-gcc-7 toolchain for the >10 years old u-boot
* curl
* the tools required by buildroot: which, sed, make, build-essential, gcc,
  g++, bash, patch, gzip, bzip2, perl, tar, cpio, unzip, rsync, file, bc, wget

Then simply run `make` in the top-level directory.

## Installation
We need to copy sdcard.img onto a microSD with something like:
```
# DANGER WILL ROBINSON DANGER! Don't overwrite the wrong device!
dd if=sdcard.img of=/dev/sdX bs=8M; sync
```
You can also use [USBImager][imager] or [balenaEtcher][etcher] to do that.

Now place the microSD in the adapter connected to the Echo and turn it on. Or if you are on a system
like Linux, where you easily can edit ext3 filesystems, you might want to consider adjusting the
config files (e.g. for Wifi) first.

Two things to know when starting from the card:

* The card's bootloader waits 2 seconds for a key on the serial console, and an unconnected
  receive pin can look like a key press. Keep the USB to UART converter connected and plugged into
  a PC, otherwise the Echo may stop at the `uboot >` prompt.
* If U-Boot reports `No partition table` or the kernel shows card errors, hand-soldered wiring
  that cannot carry fast 4-bit transfers is a likely cause. [mmc/README.md](../mmc/README.md)
  describes it and has a card setup for 1-bit mode that works with such wiring.

## Installation on internal storage
On your microSD are 3 partitions, one for booting and two times the rootfs. The second rootfs will
be flashed to the Echo by running the installation script after booting from microSD:
```
# DANGER WILL ROBINSON DANGER! This will overwrite your original Echo firmware! Make a Backup first!
./install.sh
```
See the Backup section below in case you want to be able to go back to your original Echo firmware!
Note that changes to your setup, like wifi configuration, the satellite's configuration and such,
won't be applied to the internal storage: the script copies the card's third partition, and you
made your changes on the second. Either copy them to the third partition before running the
script, or mount `/dev/mmcblk1p6` somewhere afterwards, copy the changes over and unmount it.

When you're done, you can boot the Echo without the microSD inserted and should end up with
the login prompt you know.

The bootloader that `install.sh` puts on the internal storage has the same 2 second wait for a key
as the card's, so an Echo without a powered serial converter may not start. The fix is a
bootloader built without the wait. That, the commands for copying your settings and the checks
after the copy are in [INSTALL.md](../INSTALL.md), steps 5 to 8. Keep the card and its wiring: it
is the only way back in if the system on the internal storage does not start.

## Login
The default `root` password is:
```
echo
```

## Wifi
To join a Wifi using __DHCP__, /etc/network/interfaces should look like:
```
auto lo
iface lo inet loopback
auto wlan0
iface wlan0 inet dhcp
	udhcpc_opts -t 16
	pre-up wpa_supplicant -B -D nl80211 -c /etc/wpa_supplicant.conf -i wlan0
	post-down killall -q wpa_supplicant
```
To join a Wifi using a __static IP address__, /etc/network/interfaces should look like:
```
auto lo
iface lo inet loopback
auto wlan0
iface wlan0 inet static
	address <Device IP, e.g. 192.168.1.123>
	netmask <Network mask, e.g. 255.255.255.0>
	gateway <Gateway/Router IP, e.g. 192.168.1.1>
	pre-up wpa_supplicant -B -D nl80211 -c /etc/wpa_supplicant.conf -i wlan0
	post-down killall -q wpa_supplicant
```
Additionally in __both__ cases /etc/wpa_supplicant.conf should look like:
```
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1

network={
	ssid="YOURSSID"
	psk=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
}
```
The `network={}` part can be generated with `wpa_passphrase "YOURSSID"`.

Now just turn it on with:
```
ifup wlan0
```
Now you can even connect to it via SSH.

## Backup
After setting up the Wifi it is recommended to make a backup of the internal storage.
For example the following command issued on a Linux host while the Echo is booted from SD card should do the trick:
```
ssh root@<IP of your Echo> dd if=/dev/mmcblk1 bs=8M | dd of=EchoBackup.dd bs=8M
```
Note that this will produce a 4GB file, but you can gzip it afterwards.
Compare `md5sum EchoBackup.dd` on the host with `busybox md5sum /dev/mmcblk1` on the Echo to be
sure the copy is complete. The file contains everything the Echo had stored, including its
credentials, so keep it private.

## Voice satellite
The image starts `echo-satellite` at boot. It needs one setting, the address of your Home
Assistant, in /etc/echo-satellite.conf:
```
allow = <IP of your Home Assistant>
```
Without it any host on your network can connect to the Echo and open the microphone. Restart it
with `/etc/init.d/S60satellite restart` (or reboot), then add the Echo in Home Assistant: Settings,
Devices & services, Add integration, ESPHome, with the Echo's IP and port 6053.
What it does is described under [Voice](VOICE.md), the Home Assistant side in [HA.md](../HA.md)
and every option in [satellite/README.md](../satellite/README.md). Its log is in the system log:
```
grep echo-satellite /var/log/messages
```

## Bluetooth
On your Smartphone switch on Bluetooth searching mode, then on the Echo execute:
```
lipc-set-prop com.doppler.btmd.bluetooth enablePhonePairing 1
```
This will make it discoverable for 5 Minutes as something like `dp-dev-0`.
Simply pair your Smartphone with it.

## MPD
Storing music on the Echo is likely a bad idea for huge collections. But using MPD there's a
[good way][archmpd] using a second device as server
(which can be the same device used for voice recognition).
On the host (which stores the Music) your /etc/mpd.conf should look like:
```
pid_file            "/run/mpd/mpd.pid"
playlist_directory  "/var/lib/mpd/playlists"
music_directory     "/path/to/your/music/"

database {
    plugin           "simple"
    path             "/var/lib/mpd/mpd.db"
    cache_directory  "/var/lib/mpd/cache"
}

audio_output {
    type  "null"
    name  "This server does not need to play music, but it can"
}
```
And on the Echo it should look like:
```
pid_file            "/run/mpd/mpd.pid"
playlist_directory  "/var/lib/mpd/playlists"

# WebDAV setup
music_directory     "http://HOSTIP:8080/"

database {
    plugin  "proxy"
    host    "HOSTIP"
    port    "6600"
}

audio_output {
        type            "pulse"
        name            "pulse audio"
}
```
Replace HOSTIP with the IP of your host. And on the host restart MPD and run:
```
rclone serve webdav --addr HOSTIP:8080 -v /path/to/your/music/
```
Now you can control the MPD on your Echo with mpc, ympd, apps like [MALP][malp] or all those other
projects around MPD.


[imager]: https://gitlab.com/bztsrc/usbimager
[etcher]: https://www.balena.io/etcher/
[archmpd]: https://wiki.archlinux.org/title/Music_Player_Daemon/Tips_and_tricks#Music_streaming_with_the_satellite_setup
[malp]: https://f-droid.org/en/packages/org.gateshipone.malp/


[![Flattr Button](http://api.flattr.com/button/button-static-50x60.png "Flattr This!")](https://flattr.com/@andre_opensource "andre_opensource")
