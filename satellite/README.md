# echo-satellite

The daemon that turns the Echo into a Home Assistant voice satellite. It
listens for Amazon's wake-word daemon (`asrd`), streams the microphone to
Home Assistant over the ESPHome native API, plays the answers, drives the LED
ring and handles the buttons. How it fits into the whole system is in the
[README](../README.md) of the repository.

**Status:** runs on Echo gen1 booted from internal storage, against a real Home Assistant: wake word, ring states,
streaming to Home Assistant, spoken answers, time, timer and device commands, the mute button and the volume ring.
Everything else was tested on a PC only, with the image's ARM binaries under
`qemu-user` and a scripted stand-in for Home Assistant (see "Testing"); what
that leaves open on the device is listed there. The whole setup sequence is
in [INSTALL.md](../INSTALL.md).

## Build and install

The image build includes the satellite: `docker/build.sh` builds it and puts
the binary, the init script and the configuration file on both partitions of
`sdcard.img`, started at boot. To build it alone and update a running Echo:

```sh
docker/build.sh make -C satellite          # needs a finished rootfs build
scp -O satellite/echo-satellite      root@<echo>:/usr/bin/
scp -O satellite/S60satellite        root@<echo>:/etc/init.d/
scp -O satellite/echo-satellite.conf root@<echo>:/etc/
ssh root@<echo> 'chmod +x /etc/init.d/S60satellite; /etc/init.d/S60satellite start'
```

`-O` because the image has no `sftp-server`. `scp` cannot replace the binary
while it runs: stop the service first.

The build takes headers from the Buildroot sysroot and links against the
libraries in `rootfs/out`, because PulseAudio, D-Bus and curl on the image are
Amazon's older builds.

`S60satellite` starts `asrd` and the satellite and restarts either if it
exits. The satellite selects the wake-word model and switches `asrd`'s result
signals on whenever `asrd` reports ready. Amazon's `dynconfig` daemon does not
have to run: `set-dynconf-value` and `asrd` use the stored values without it
(checked under emulation).

Run by hand with `echo-satellite -v` for debug output on the terminal; `-s`
logs to syslog, `-o key=value` overrides the configuration file.

## Configuration

Everything has a default; [echo-satellite.conf](echo-satellite.conf) lists all
options. The two worth setting:

- `allow = <address of Home Assistant>`. The API is unencrypted and
  unauthenticated, and a client can open the microphone (it can ask a question
  and receive the reply audio). Without `allow`, any host on the network can.
- `mdns_file = /etc/avahi/services/echo-satellite.service`, so Home Assistant
  discovers the device. Otherwise add it by address.

## Home Assistant

Settings → Devices & services → Add integration → ESPHome, host = the Echo's
address, port 6053, no encryption key. The device appears with an Assist
satellite entity, the entities below, and Home Assistant's own pipeline, wake
word and "finished speaking detection" settings. See [HA.md](../HA.md).

| Entity | Type | What it does |
|---|---|---|
| Media player | media player | Volume and speaker mute, `play_media` (stop, no pause), announcements |
| Mute | switch | On: the microphones are off, as after a press of the mute button. Follows the button and can be switched from Home Assistant |
| Timer ringing | binary sensor | On while a finished timer rings |
| Stop ringing | button | Silences a ringing timer, also one that waits for a conversation to end. Does nothing else |

A satellite that was added before these entities existed gets them the next
time Home Assistant connects (restart the satellite or reload the
integration).

What the device declares: voice assistant with audio over the API connection,
timers, announcements, start conversation; no speaker entity, so Home
Assistant hands over URLs and the device fetches them. It asks for WAV, mono,
16 bit, 48 kHz (the sink's rate), which makes Home Assistant do the decoding
and resampling.

## Behaviour

| Event | Action | Ring (default) |
|---|---|---|
| Wake word | Request to Home Assistant; audio is streamed from the sample where the wake word ended, taken from the DSP ring buffer | `active_start`, then `alexa_user-talking` |
| End of speech (Home Assistant decides) | Stop streaming | `alexa_thinking` |
| Response URL | Play while downloading; with streaming TTS playback starts before the text is complete | `dialog-talk` |
| Response says "continue conversation" | Listen again without wake word | |
| Announcement, question from Home Assistant | Play chime and message; for a question, listen afterwards | `dialog-talk` |
| Timer finished | Ring until silenced or `ring_seconds` pass; if a conversation is going on, ring after it | `ready_timer` |
| Wake word while something plays or rings | Silence it and listen | |
| "Computer, stop" (`asrd` hears "stop" locally) | The wake word silenced the sound; the request it opened is dropped | |
| Action button | Silence ring or playback, abort a request, or start a request without wake word | |
| Mute button | Hardware mic cut by `buttond`; sound and ring pattern here | `mics-off_on` |
| "Mute" switch in Home Assistant | `buttond` is asked to act as if the button was pressed; no sound, the ring shows it | `mics-off_on` |
| Wake word while muted | Ignored (cannot happen while the hardware cut works) | |
| "Stop ringing" button in Home Assistant | Silence a ringing timer | |
| Error from Home Assistant, timeout | Back to idle after 1.5 s | `error` |

Volume ring and speaker volume stay with the stock `audioCtrl`. The media
player entity forwards volume and mute from Home Assistant to it and plays
URLs (stop, no pause).

### Microphone level

The DSP's output is quiet: in five commands recorded by Home Assistant, speech
peaked at -28 to -40 dBFS. `mic_gain_db` (default 20) amplifies the samples
before they are sent, with limiting. Parakeet and Whisper gave almost the same
text with and without it (one word of five commands improved), so this is for
headroom and for Home Assistant's end-of-speech detection more than for
accuracy.

The preamplifiers in front of the DSP are no remedy: raising the four
`MICPGA` controls from 40 to 60 raised the noise by 9 dB together with the
speech. `tools/sndctl` (`docker/build.sh make -C satellite tools`) lists and
sets the sound card's controls; the image has no `amixer`. The stock value
returns at the next boot.

### Wake word

Alexa by default (`wake_word`); Home Assistant can change it among Alexa,
Computer, Echo and Amazon, and the choice is kept in `state_file`.

`asrd` loads the model named by the dynconfig value `asrd.model` when it
starts. To select a model the satellite

1. stores it: `set-dynconf-value asrd.model <model_dir>/<locale>/<WORD>/pryon.manifest`,
2. writes `asrd`'s `WWModel` property, which switches the model at run time
   under emulation,
3. looks again 6 s later, and if `asrd` still runs another model, runs
   `asr_restart_cmd` (`killall asrd`); the init script starts `asrd` again
   and it loads the stored model.

Step 3 exists because a real Echo refused the property write
(`lipcErrInvalidArg`), while storing the value and restarting `asrd` worked
there. `asr_runtime_switch = 0` skips step 2 and restarts right away. After
two restarts without the wanted model the satellite gives up with a warning.

Policy choices: an announcement interrupts a request in progress; a wake word
interrupts anything; media does not start while the satellite is busy.

## Interfaces to the stock daemons

Observed with `dbus-monitor` on the image's own binaries. All calls and
signals use the service name as interface, path `/default`.

| What | Form |
|---|---|
| Set a property | method `set<Prop>Str(string)` or `set<Prop>Int(int32)` |
| Get a property | method `get<Prop>Str()` / `get<Prop>Int()`, reply `(uint32 status, value)`, status 0 = ok |
| `com.doppler.buttond` signal `button` | `(string name, int32 state)`: `uber` 1 = down, 2 = up; `mute` 6 = mics off, 7 = on; `volume` 1 = down, 2 = up. Property `muteState` holds 6 or 7. |
| `com.doppler.buttond` property `fakeButton` (write only) | `"<button> <state>"` with the state names of `button_config.json`, e.g. `mute enabled`, `mute disabled`. `buttond` then does what it does for the real button: it pulses the GPIO line of that state (195 mutes, 193 unmutes), stores the state and sends the `button` signal. Under emulation there are no GPIO lines; state and signal were seen there, the pulse only as a failed attempt in `buttond`'s log. Writing `muteState` instead changes the stored value without the pulse. |
| `com.doppler.audiod` (audioCtrl) | properties `MainVolume` 0–100 and `Mute`; signals `MainVolumeChanged(int32 volume, int32 source)`, `MuteChanged(int32 mute, int32 volume)` |
| `com.doppler.asr` | as in [ASRD.md](../ASRD.md). Writing `WWModel` (e.g. `en-US/ALEXA`) switches the model at run time under emulation (`asrd` reports not ready for a moment and then ready); on a real Echo the write was refused. |
| `libshmbuffer.so` | prototypes recovered from the disassembly, see `mic.c` |

"down" for the action button is the state the stock `button_config.json` maps
to key value 0. Whether that is press or release on the real board is not
verified; if the button reacts on release, that is the reason.

## Testing

`test/run.sh` runs the daemon in an ARM chroot of the image together with the
real `asrd`, `buttond`, `audioCtrl` and PulseAudio. Speech clips are written
into `/BMicsOut.shm` in real time, button events into the FIFOs `buttond`
reads, and `test/fake_ha.py` plays Home Assistant using `aioesphomeapi`, the
client library Home Assistant itself uses. Prerequisites are in the header of
`run.sh` and `mkfixtures.py`.

Passing, `test/main.sh`, `test/startup.sh` and `test/model.sh`:

- wake word → request → streamed audio → response played. The audio Home
  Assistant received starts at the first sample after the wake word and is
  bit-identical to the input.
- streamed response (URL at run start, header and data arriving in pieces)
- announcement with chime, announcement whose file is missing (reported as
  failed), question followed by listening
- timer ringing, silenced by button and by "computer, stop"; timer finishing
  during a conversation
- action button, mute button, volume from Home Assistant and from the ring
- media playback interrupted by the wake word
- "Mute" switch from Home Assistant: on and off, no sound, the state
  reported back; a wake word while muted is ignored; muted by the button and
  unmuted from Home Assistant
- "Timer ringing" on and off for every way a ring ends; "Stop ringing" from
  Home Assistant, with a timer ringing and with none
- wake word changed from Home Assistant: the new word triggers, the old one
  does not; the same with the run-time switch turned off, so that the model
  is stored and `asrd` restarted, with and without the `dynconfig` daemon
- connection dropped while streaming, reconnect
- daemon started before `asrd`; client restricted by `allow`; a client that
  wants encryption is turned away with a clear log message
- init script: start, restart after `kill -9`, stop

`test/fuzz.c` feeds the API parser random and malformed frames in a native
build with AddressSanitizer and UBSan; no findings in over 600,000 frames
(210,000 again after the switch and button commands were added).

On the device with a real Home Assistant: wake word "Alexa",
the ring states including the error pattern, voice commands answered through
the speaker (time, timers, switching a device), start at boot through the
init script. A command took about 2.5 s from the wake word to the first
audio of the answer, the "Mute" switch,  "Timer ringing" and "Stop ringing" from Home Assistant.

The mute button and the volume ring work there too (reported from daily use,
not from a test run).

Not tried on the device:

- changing the wake word from Home Assistant, and the `asrd` restart it
  needs there
- announcements, questions, a timer ringing and being silenced
- echo cancellation while the device speaks, CPU load
- `/etc/init.d/S60satellite stop` and `restart`, and `reboot`
- HTTPS URLs

## Limitations

- No Noise encryption on the API connection. Use `allow` and a trusted LAN.
- WAV (PCM 16 bit) only. Home Assistant converts everything else; a URL that
  bypasses its proxy and is not WAV fails.
- One sound at a time: no ducking of media under speech.
- Stopping a response whose download has stalled can take up to a second.
- Home Assistant's wake-word verification (device sends the audio before the
  wake word) is not implemented, although the ring buffer has that audio.
- Timers show nothing while they run; only the ringing is implemented.
