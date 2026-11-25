# asrd: reusing Amazon's wake-word engine

`asrd` is the stock daemon that runs Amazon's wake-word engine (`libpryon.so`)
on the Echo gen 1. The image starts it together with the voice satellite
(`/etc/init.d/S60satellite`). This document
describes how to run it on its own, without any other Alexa service and
without network access, and how to consume its results.

**Status:** everything here was observed on a PC, running the image's ARM
binaries in a chroot under `qemu-arm` and then successfully tested on Echo,
started by the satellite's init script ([satellite/README.md](satellite/README.md)),
with the ALEXA model in daily use.

## Summary

- `asrd` reads the DSP's 16 kHz mono output from `/BMicsOut.shm` and looks for
  one wake word.
- It reports detections as D-Bus signals. It does not pass audio on; a signal
  carries a position in `/BMicsOut.shm`, and the consumer reads the audio from
  there.
- Models on the image: ALEXA, COMPUTER, ECHO, AMAZON, each for en-US, en-GB,
  de-DE and es-US.
- After the wake word it also listens briefly for the word "stop" and reports
  that locally.
- It needs four small things to run standalone; see "Requirements".

## Files

| Path | What |
|---|---|
| `/usr/local/bin/asrd` | The daemon |
| `/usr/local/lib/libpryon.so` | Wake-word engine (19 MB, Kaldi based) |
| `/usr/local/data/models/keyword/doppler/<locale>/<WORD>/` | Wake-word models; entry point is `pryon.manifest` |
| `/usr/local/data/models/AED/` | Acoustic event model, loaded at start, otherwise unused here |
| `/usr/local/bin/dynconfig`, `get-dynconf-value`, `set-dynconf-value` | Config store that holds the model choice |
| `/usr/local/bin/lipc-get-prop`, `lipc-set-prop`, `lipc-probe`, `lipc-wait-event` | Command-line access to lipc |
| `/etc/init/asrd.conf`, `/usr/local/bin/updateAsrdDynConfig.sh` | Stock upstart job and its pre-start script (reference only; echoroot does not use upstart) |

## Starting it

```
asrd [options] audio_shm_name asr_blob_shm_name model_config_file
```

The stock job runs `asrd /BMicsOut.shm /BAsrMetadata.shm`.

- `audio_shm_name`: the mic ring buffer to read. Echoroot's `S30misto`
  already creates `/BMicsOut.shm` and `audioproxyd` fills it.
- `asr_blob_shm_name`: a buffer `asrd` writes per-detection metadata into.
  `S30misto` already creates `/BAsrMetadata.shm`. The stock firmware sent this
  to the cloud; we do not need it.
- `model_config_file`: accepted but does not select the model. Passing the
  COMPUTER manifest here still loaded ALEXA.

It logs to syslog only, with the tag `ASRD`.

### Requirements

1. **`/proc/board_rev`.** `asrd` exits with `Bad revision file` without it.
   The stock kernel provides it. In the chroot a fake value `EVT-3.2` worked;
   the kernel's known Echo values are `EVT-2.0`, `EVT-2.1`, `EVT-3.0`,
   `EVT-3.1`, `EVT-3.2`.
2. **`libdavs.so.1`.** `asrd` is linked against that name, the image only has
   `libdavs.so`:
   `ln -s libdavs.so /usr/lib/libdavs.so.1`
3. **A usable locale.** If `LANG` names a locale the image does not have,
   model loading fails with `locale::facet::_S_create_c_locale name not
   valid`. Unset `LANG` or set `LC_ALL=C`.
4. **D-Bus system bus.** Already started by the image (`S30dbus`).

### Choosing the wake word

The model comes from the `dynconfig` key `asrd.model`. If the lookup fails,
`asrd` falls back to `en-US/ALEXA`, so ALEXA needs nothing extra.

For any other word, `dynconfig` must be running and the key set before `asrd`
starts:

```sh
/usr/local/bin/dynconfig &
set-dynconf-value asrd.model \
  /usr/local/data/models/keyword/doppler/en-US/COMPUTER/pryon.manifest
```

On a real Echo `set-dynconf-value asrd.model <manifest>` followed
by a start of `asrd` worked: "Computer" triggered. Whether the `dynconfig`
daemon was running at that moment was not recorded; under emulation it is not
needed for this. Without a stored value `asrd` came up with the Alexa model.

`asrd` also has a writable `WWModel` property. Under emulation, writing it
switches the model at run time: `lipc-set-prop com.doppler.asr WWModel
en-US/ALEXA`. About three seconds later `asrd` signals `ReadyChanged` `No`,
then `Yes`, and from then on only the new word triggers. The satellite enables
`Broadcast` again at that point (whether the switch resets it was not
checked).

On the real Echo the same write (`en-US/COMPUTER`) was refused with
`lipcErrInvalidArg`. The reason was not found; the binary has an error path
for a failed dynconfig write in this setter. The satellite therefore stores
the value with `set-dynconf-value` and, if the property write has no effect,
has `asrd` restarted.

### Enabling results

`asrd` starts with its `Broadcast` property set to `Disabled`. In that state
it detects and logs wake words (`reason=recognitionDisabledByBroadcast`) but
sends no signal. Enable it once `Ready` is `Yes`:

```sh
lipc-set-prop com.doppler.asr Broadcast Enabled
```

The value resets on every start of `asrd`. In the stock firmware another
daemon owned this switch; here the satellite has to.

## How it reports results

Amazon's "lipc" is a thin layer over the D-Bus system bus. `asrd` owns the bus
name `com.doppler.asr`. Events are D-Bus signals with path `/default` and
interface `com.doppler.asr`.

### Signals

| Signal | When | Arguments |
|---|---|---|
| `FoundKeyword` | Wake word accepted | Nine, see below |
| `FoundCmd` | Local command heard after the wake word | Five, see below |
| `StateChanged` | State machine moves | One string |
| `ReadyChanged` | Models loaded | `Yes` / `No` |
| `WWModelChanged` | Model selected | e.g. `en-US/COMPUTER` |
| `BroadcastChanged` | `Broadcast` property changed | `0` / `1` |
| `metricWakeWordDetected`, `metricLocalCommandDetected` | With each detection | Counter and request id; for Amazon's metrics, ignore |

States: `waitingForWakeword` → `waitingForLocalCmdOrTimeout` on a wake word →
back to `waitingForWakeword`, either through `localCmdTimeout` or directly
after a local command.

### FoundKeyword

Example, as printed by `lipc-wait-event`, with the matching syslog line:

```
FoundKeyword 29760 561429125 1795230329273 8000 20480 1000 "14a4b233-..." 0 1400
I ASRD:sendWakeUpWordwithSeqNum_locked:WakeupWord=COMPUTER,beginIndex=37760,endIndex=50240,...
```

| # | Example | Meaning |
|---|---|---|
| 1 | 29760 | Sample sequence number in `/BMicsOut.shm` where the utterance stream starts: wake word start minus the pre-roll |
| 2 | 561429125 | Monotonic counter |
| 3 | 1795230329273 | Wall-clock time in ms |
| 4 | 8000 | Wake word start, in samples from #1 (the 500 ms pre-roll) |
| 5 | 20480 | Wake word end, in samples from #1 |
| 6 | 1000 | Unknown |
| 7 | "14a4b233-..." | Request id, shared with a following `FoundCmd` |
| 8 | 0 | Probably the offset of this detection's blob in `/BAsrMetadata.shm` |
| 9 | 1400 | Probably that blob's length |

All arguments are `uint64` except the string. The meanings are inferred from
the values across several detections and from the log lines; Amazon does not
document them. #1, #4 and #5 are consistent in every detection observed
(`#1 + #4 = beginIndex`, `#1 + #5 = endIndex`).

To stream a command without making the user pause after the wake word, set the
reader position to `#1 + #5` with `ShmbufSetReaderSeqNum` (exported by
`libshmbuffer.so`) and read from there.

### FoundCmd

```
FoundCmd "STOP" 253440 273600 284320 "4d990d2b-..."
```

Command name, the stream start from the preceding `FoundKeyword`, the
command's begin and end as absolute sample numbers, and the request id. It is
recognised only in the `waitingForLocalCmdOrTimeout` window after a wake word,
and it has its own switch, the `StopBroadcast` property, which was already
`Enabled` in testing.

STOP is the only local command, and no others can be configured. Checked
across all 16 model directories on the image:

- The vocabulary files (`words.shrunk.txt`) contain only the four wake words,
  `STOP` and background filler entries.
- The state machines (`op.cfg.json`) have rules only for the wake word and
  `STOP`.
- The only decoding graphs (`*.hclg.pfst`) are for the wake words and `STOP`.

The engine is a keyword spotter, not a speech recogniser. Each word needs its
own graph and classifier, built with Amazon's tools, so adding "timer" or
"time" is not practical.

On the original Echo, "what time is it" and "set a timer" were recognised in
Amazon's cloud. Only the ringing was local (the `alarmd` daemon), and STOP
exists so that an alarm or timer can be silenced without the cloud. Here the
same split applies: Home Assistant handles time and timer commands, and
`FoundCmd STOP` is the local way to silence a ringing timer.

### Thresholds change with device state

The models' `kw.cfg.json` contains threshold overrides keyed on "client
properties" named `AudioPlayerState` and `AlarmState`: when either is 1, the
accept threshold drops from -1 to -4, making the wake word easier to trigger
while music plays or an alarm rings. `asrd` reads the matching lipc properties
from other stock daemons (`com.doppler.player AudioPlayerState`,
`com.doppler.alarmd Status` and others). Those daemons do not run here, so
`asrd` logs a lookup error for each at start and uses the default thresholds.
Whether the satellite could publish these properties itself to get the stock
behaviour is untested.

### Properties

From `lipc-probe -a com.doppler.asr`:

| Property | Access | Notes |
|---|---|---|
| `Broadcast` | rw | `Enabled` / `Disabled`. Gates `FoundKeyword`. |
| `StopBroadcast` | rw | Same for `FoundCmd`. |
| `State` | r | Current state, as in `StateChanged`. |
| `Ready` | r | `Yes` once models are loaded. |
| `WWModel` | rw | e.g. `en-US/COMPUTER`. Writing switches the model, see "Choosing the wake word". |
| `IgnoreRecognitionRange` | w | Untested. Probably excludes a sample range, such as the device's own speech. |
| `Restart` | w | Untested. |
| `logLevel`, `logMask` | rw | Logging. |
| `AcousticEventDetection`, `AcousticEventBroadcast`, `AcousticEventType`, `AcousticEventSupportedTypes`, `AEDECIDs`, `FingerprintECIDs`, `Davs` | mixed | Glass-break style event detection and model download state. Not needed. |

## Consuming it

From a shell, one line per event:

```sh
lipc-wait-event -m com.doppler.asr FoundKeyword,FoundCmd,StateChanged
```

or with plain D-Bus tools:

```sh
dbus-monitor --system "interface='com.doppler.asr'"
```

From C, add a match rule with libdbus and read the arguments in order:

```
type='signal',interface='com.doppler.asr',member='FoundKeyword'
```

## Running it as a service

The stock firmware ran `asrd` as an upstart job with respawn. Echoroot uses
busybox init scripts; `satellite/S60satellite` starts `asrd` after
`audioproxyd` and restarts it when it exits, and the satellite selects the
model and enables the signals. Without the satellite the same takes:

```sh
ln -sf libdavs.so /usr/lib/libdavs.so.1
/usr/local/bin/dynconfig &
set-dynconf-value asrd.model \
  /usr/local/data/models/keyword/doppler/en-US/COMPUTER/pryon.manifest
LC_ALL=C /usr/local/bin/asrd /BMicsOut.shm /BAsrMetadata.shm &
until [ "$(lipc-get-prop com.doppler.asr Ready)" = "Yes" ]; do sleep 0.25; done
lipc-set-prop com.doppler.asr Broadcast Enabled
```

For real use:

- **Respawn.** Init scripts do not restart a crashed process. Use a
  `::respawn:` line in `/etc/inittab` or a supervising loop.
- **`Broadcast` after a restart.** Have the satellite set it whenever it sees
  `ReadyChanged` with `Yes`.
- **Priority.** `asrd` asks for real-time scheduling. That failed in the
  chroot (no privilege) and should succeed as root on the device.

## Test results

Input: three synthetic voices (Piper `en_US-lessac`, `en_US-ryan`,
`en_GB-alba`), each saying "Computer, what is the time?", "Alexa, what is the
weather today?", "I would like to commute to the conference by train." and
"Computer, stop.", with 2.5 s of silence in between. Clean audio, no room, no
noise.

| Model | Expected | Detected | False triggers |
|---|---|---|---|
| ALEXA | 3 | 3 | 0 |
| COMPUTER | 6 | 6 | 0 |
| STOP after COMPUTER | 3 | 3 | 0 |

ALEXA was also detected in Porcupine's `multiple_keywords.wav`, a human
recording. Classifier scores for ALEXA were 0.91 to 1.00 against a threshold
of 0.57.

`asrd` logged `pryon_latency_msec` values of 200 to 270. What that interval
covers is not documented, and the figure comes from emulation, so do not read
it as on-device latency.

Network: the COMPUTER test was repeated in an empty network namespace with the
same six detections, and `asrd` opened no internet sockets in a traced
60-second run. It does link Amazon's download and metrics libraries and starts
a download worker thread, so this should be re-checked on the device with
`tcpdump`.

## How this was verified

The method, for repeating or extending the tests on a PC with `qemu-user-static`
registered in binfmt_misc:

1. Extract the rootfs partition from `sdcard.img` (partition 2, starts at
   sector 32769) and unpack it without root:
   `debugfs -R "rdump / root" rootfs.ext3`
2. Enter a user, mount and PID namespace (`unshare -rmpf`) and prepare the
   tree before `chroot`:
   - tmpfs on `root/dev/shm` and on `root/mnt/volatile` (the image's
     `/var/run` and `/tmp` point there)
   - tmpfs on `root/proc` containing `board_rev` with `EVT-3.2`
   - bind mounts of the host's `/dev/null`, `zero`, `urandom`, `random`
3. Inside the chroot:
   - link `/usr/local/lib/*` into `/usr/lib` as `S30misto` does, plus the
     `libdavs.so.1` link
   - start `syslogd -O <file>` to capture `asrd`'s log
   - start `dbus-daemon` with a copy of `/etc/dbus-1/system.conf` that has the
     `<user>` line removed
   - create the buffers:
     `shmbuf_tool -m 0 -S /BMicsOut.shm -n 1048575` and
     `shmbuf_tool -m 0 -S /BAsrMetadata.shm -s 1 -n 131072`
   - start `dynconfig`, set `asrd.model`, start `asrd`, enable `Broadcast`
   - play raw 16 kHz S16LE mono audio into the mic buffer in real time:
     `shmbuf_tool -m 3 -S /BMicsOut.shm -i audio.raw -t 8000 -w 128`
4. Read the results from the syslog file and from `dbus-monitor --system` or
   `lipc-wait-event`.

Setting `QEMU_STRACE=1` for `asrd` prints its system calls, which is how the
`/proc/board_rev` and locale requirements were found.

## Open questions

- CPU and memory on the Cortex-A8, alongside `audioproxyd`.
- Hit rate at distance and false triggers from TV and conversation, for
  COMPUTER and ALEXA, on the DSP's real output.
- Whether `asrd` attempts network access on the device.
- Whether `dynconfig` keeps `asrd.model` across reboots.
- The unknown `FoundKeyword` arguments and the write-only properties.
- Linking `libpryon.so` directly instead of running `asrd`. The library
  exports a C API (`PryonModelSet_New`, `PryonDecoder_NewPcmInt16`,
  `PryonDecoder_PushAudioEventSamples`, `PryonApi_Set*Callback`), but the
  signatures would have to be recovered by disassembly. Only worth it if
  `asrd` proves too heavy.
