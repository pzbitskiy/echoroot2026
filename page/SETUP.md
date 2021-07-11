# The software part

## Building
In case you want to build it yourself instead of using prebuilt images, you'll need:

* GNU Awk >= 4.2.1 or mawk >= 1.3.3
* binutils >= 2.34
* An arm-linux-gnueabihf-gcc-7 toolchain for the >10 years old u-boot
* curl
* About 15GB disk space

Patches are welcome for support of other versions.
You also need tools required by buildroot:

* which
* sed
* make (version 3.81 or any later)
* binutils
* build-essential (only for Debian based systems)
* gcc (version 4.8 or any later)
* g++ (version 4.8 or any later)
* bash
* patch
* gzip
* bzip2
* perl (version 5.8.7 or any later)
* tar
* cpio
* unzip
* rsync
* file (must be in /usr/bin/file)
* bc
* wget

Now simply run `make` in the top-level directory

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

## Status
Currently the system only is intended to run from the external SD card.
I plan to add installation instructions to make it the main OS on the internal storage.

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
