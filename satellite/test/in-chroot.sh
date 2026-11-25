#!/bin/sh
# Inside the ARM chroot: stock daemons, echo-satellite, then the scenario $1.
# Environment: SAT_OPTS (extra daemon options), SAT_BEFORE_ASRD=1,
# NO_DYNCONF=1 (asrd then starts with its default model), ASRD_LOOP=1 (asrd
# is restarted when it exits). The daemon runs with wake_word=computer unless
# SAT_OPTS says otherwise; the harness preselects that model in dynconfig.
# The image's busybox has no head, tail, cut, sort or tr.
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
unset LANG LANGUAGE LC_CTYPE LC_MESSAGES; export LC_ALL=C HOME=/tmp
T=/media/t; OUT=$T/out; FX=$T/fixtures
SAT="$OUT/echo-satellite -v -o pulse_server=unix:/tmp/pulse.sock -o state_file=/tmp/sat.state -o wake_word=computer $SAT_OPTS"
ln -sf /usr/local/lib/* /usr/lib/ 2>/dev/null
ln -sf libdavs.so /usr/lib/libdavs.so.1
mkdir -p /mnt/volatile/run/dbus /mnt/volatile/log /mnt/volatile/tmp /mnt/volatile/cache /mnt/volatile/lock
syslogd -O $OUT/messages
sed -e '/<user>/d' /etc/dbus-1/system.conf > /etc/dbus-1/system-test.conf
dbus-daemon --config-file=/etc/dbus-1/system-test.conf
sleep 1
dbus-monitor --system > $OUT/dbus.log 2>&1 &
shmbuf_tool -m 0 -S /BMicsOut.shm -n 1048575 >/dev/null
shmbuf_tool -m 0 -S /BAsrMetadata.shm -s 1 -n 131072 >/dev/null
shmq_tool -m 0 -S /AudiodCmd.shm -n 3000 >/dev/null
# buttond reads its input devices from these; the scenario writes events into them
mkfifo /dev/input/event0 /dev/input/event1
exec 3<>/dev/input/event0
exec 4<>/dev/input/event1
# A null sink keeps real time; its monitor records what would reach the speaker.
pulseaudio -n --daemonize=no --exit-idle-time=-1 \
  -L "module-native-protocol-unix auth-anonymous=1 socket=/tmp/pulse.sock" \
  -L "module-null-sink sink_name=lab126-sink format=s16le rate=48000 channels=2" \
  > $OUT/pulse.log 2>&1 &
buttond > $OUT/buttond.log 2>&1 &
sleep 2
PULSE_SERVER=unix:/tmp/pulse.sock parec -d lab126-sink.monitor --raw --format=s16le --rate=48000 --channels=2 > $OUT/sink.raw 2>$OUT/parec.log &
audioCtrl /AudiodCmd.shm > $OUT/audioctrl.log 2>&1 &
if [ -z "$NO_DYNCONF" ]; then
  dynconfig > $OUT/dynconfig.log 2>&1 &
  sleep 2
  set-dynconf-value asrd.model /usr/local/data/models/keyword/doppler/en-US/COMPUTER/pryon.manifest >/dev/null
fi
if [ -n "$SAT_BEFORE_ASRD" ]; then $SAT >> $OUT/sat.log 2>&1 & SPID=$!; fi
if [ -n "$ASRD_LOOP" ]; then   # restarted when it exits, as S60satellite does
  # killall cannot work here (no /proc), so the scenario passes
  # asr_restart_cmd=/tmp/restart-asrd
  printf '#!/bin/sh\nkill $(cat /tmp/asrd.pid)\n' > /tmp/restart-asrd; chmod +x /tmp/restart-asrd
  ( while :; do
      asrd /BMicsOut.shm /BAsrMetadata.shm >> $OUT/asrd.log 2>&1 &
      echo $! > /tmp/asrd.pid; wait $!
      echo "=== asrd exited" >> $OUT/asrd.log; sleep 2
    done ) &
else
  asrd /BMicsOut.shm /BAsrMetadata.shm > $OUT/asrd.log 2>&1 &
fi
sleep 7
if [ -z "$SAT_BEFORE_ASRD" ]; then $SAT >> $OUT/sat.log 2>&1 & SPID=$!; fi
i=0; while [ ! -f $OUT/ha.connected ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
[ -f $OUT/ha.connected ] || echo "[t] fake HA never connected"

mark()  { echo "[t] $*"; echo "=== $*" >> $OUT/sat.log; }
say()   { shmbuf_tool -m 3 -S /BMicsOut.shm -i $FX/audio/$1 -t 8000 -w 128 >/dev/null; }   # real time
quiet() { dd if=/dev/zero bs=32000 count=$1 2>/dev/null > /tmp/sil.raw
          shmbuf_tool -m 3 -S /BMicsOut.shm -i /tmp/sil.raw -t 8000 -w 128 >/dev/null; }
cmd()   { : > $OUT/cmd.$1; }                 # ask fake_ha.py to do something
key()   { cat $FX/$1.bin >&3; }              # action and mute buttons
ring()  { cat $FX/$1.bin >&4; }              # volume ring
. $T/$1
sleep 1
kill -TERM $SPID; sleep 1
kill -0 $SPID 2>/dev/null && echo "[t] satellite did not exit" || echo "[t] satellite exited"
