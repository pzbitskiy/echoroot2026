#!/bin/sh
# Runs echo-satellite with the image's own asrd, buttond, audioCtrl and
# PulseAudio in an ARM chroot under qemu-user, against fake_ha.py, which
# plays Home Assistant through the aioesphomeapi client library.
#
#   ROOT=<extracted image root> PY=<python with aioesphomeapi> ./run.sh main.sh
#
# ROOT: partition 2 of sdcard.img unpacked without root, see ASRD.md,
#       "How this was verified". Needs qemu-user-static in binfmt_misc and
#       unprivileged user namespaces.
# Needs ./mkfixtures.py run once and four speech clips, see its header.
# Results: out/sat.log (daemon), out/ha.log (fake Home Assistant),
#          out/dbus.log (all bus traffic), out/sink.raw (speaker, 48 kHz stereo).
T=$(cd "$(dirname "$0")" && pwd)
: "${ROOT:?set ROOT to the extracted root filesystem}"
PY=${PY:-python3}
SCENARIO=${1:?usage: run.sh <scenario.sh>}
rm -rf "$T/out"; mkdir -p "$T/out" "$ROOT/dev/input" "$ROOT/media/t"
rm -f "$ROOT/dev/input/event0" "$ROOT/dev/input/event1"
cp "$T/../echo-satellite" "$T/../S60satellite" "$T/../echo-satellite.conf" "$T/out/" || exit 1
exec unshare -rmpfn sh -c "
  ip link set lo up &&
  mount -t tmpfs tmpfs $ROOT/proc && echo EVT-3.2 > $ROOT/proc/board_rev &&
  mount -t tmpfs tmpfs $ROOT/dev/shm &&
  mount -t tmpfs tmpfs $ROOT/mnt/volatile &&
  mount --bind $T $ROOT/media/t &&
  for n in null zero urandom random; do mount --bind /dev/\$n $ROOT/dev/\$n; done &&
  ( $PY -I $T/fake_ha.py $T/out $T/fixtures/www > $T/out/ha.stderr 2>&1 & ) &&
  exec chroot $ROOT /bin/sh /media/t/in-chroot.sh $SCENARIO
"
