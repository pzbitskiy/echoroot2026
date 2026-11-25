# Voice I/O

Since 2026 the image is a voice satellite for [Home Assistant][ha] out of the box: Amazon's own
wake-word engine listens on the Echo, and what you say after the wake word goes to your Home
Assistant, which answers through the Echo's speaker. Nothing is sent to Amazon. The original
Porcupine tools are still on the image and are described at the end.

## Output
espeak is preinstalled, and at best used with piping it to pulseaudio:
```
espeak --stdout "hello world" | paplay
```
Adjust the volume by turning the volume dial on the top.

With the satellite running, Home Assistant can also make the Echo speak or play a sound, with the
`assist_satellite.announce` and `media_player.play_media` actions. See [HA.md](../HA.md).

## Input
The seven microphones are combined by the DSP into one beamformed and echo-cancelled signal,
16kHz mono, which every program reads from the shared buffer `/BMicsOut.shm`. Two programs are
started at boot by `/etc/init.d/S60satellite`:

* `asrd`, Amazon's wake-word engine from the original firmware, running on its own without any
  other Alexa service and without network access. It only reports that it heard the wake word and
  where in the buffer. Details are in [ASRD.md](../ASRD.md).
* `echo-satellite`, which reacts to that report, streams the audio that follows to Home Assistant,
  plays the answer and drives the LED ring like the original firmware did. Details are in
  [satellite/README.md](../satellite/README.md).

What you can do with it:

* Say "Alexa" and a command. The wake word can be changed to "Computer", "Echo" or "Amazon" in
  Home Assistant, on the device's page.
* Home Assistant decides what the sentence means: time, timers, weather and your devices are
  handled by Home Assistant itself, and it can pass other questions to a language model. Speech
  recognition and speech synthesis run on machines of your own. See [HA.md](../HA.md) and the
  [ha folder](../ha/README.md).
* A finished timer rings on the Echo. Say the wake word and "stop", or press the action button, to silence it.
* The action button also starts a request without the wake word and stops an answer.
* The mute button cuts the microphones and turns the ring red. Home Assistant shows that as a
  switch and can switch it too.

The only setting it needs is the address of your Home Assistant, see [Setup](SETUP.md). By default
it is silent when it starts listening and shows that on the ring only. To get Amazon's sounds for
the start and the end of a request, add to /etc/echo-satellite.conf:
```
sound_wake = /usr/local/share/earcon/doppler/base/system_state_active_start.wav
sound_done = /usr/local/share/earcon/doppler/base/system_state_user_speech_confirmed.wav
```
The ring patterns can be changed there too.

Hint: Other LED patterns can be found on the Echo in /usr/local/etc/ledd

## Without Home Assistant: the Porcupine tools
This is the original voice input of this project, from before the satellite: wake-word detection
with [Porcupine][porcupine] and the audio sent to another machine as a raw stream. The satellite
does not use it. Two applications are preinstalled:

* porcupine_demo_shm
* porcupine_send_shm

Stop the satellite first, so that only one program reacts to the wake word and drives the ring:
```
/etc/init.d/S60satellite stop
```

### porcupine_demo_shm
porcupine_demo_shm is really just a demo. It waits for the wake-word and says "hi" if it's detected.
You can run it like:
```
porcupine_demo_shm /usr/lib/libpv_porcupine.so /usr/share/porcupine/porcupine_params.pv /usr/share/porcupine/keyword_files/beaglebone/picovoice_beaglebone.ppn 0.5 < /tmp/spy &
/usr/local/bin/shmbuf_tool -m 2 -s 1 -S BMicsOut.shm -o /tmp/spy
```
Now say "picovoice" and receive a "hi" from the speaker.

### porcupine_send_shm
porcupine_send_shm sends some seconds of audio via tcp/ip on wake-word detection. It can also be
modified to enable multiple wake-words, other LED ring patterns, and other time limits.
For a first try run this on the host:
```
nc -dNlp 1338 | cat voice/5s.wavheader - > echo.wav
```
And this on the Echo:
```
porcupine_send_shm /usr/lib/libpv_porcupine.so /usr/share/porcupine/porcupine_params.pv /usr/share/porcupine/keyword_files/beaglebone/picovoice_beaglebone.ppn 0.5 HOSTIP 1338 < /tmp/spy &
/usr/local/bin/shmbuf_tool -m 2 -s 1 -S BMicsOut.shm -o /tmp/spy
```
Now after saying "picovoice" the LED ring turns on and you can say a sentence. It will be stored on
the host in echo.wav. It might be too quite to hear it without turning up the volume though, but
that doesn't matter much for voice recognition.

I recommend to try [voice2json][voice2json]. After setting voice2json up according to the
voice2json documentation you can run this on the host:
```
nc -dNlp 13388 | cat voice/5s.wavheader - | voice2json transcribe-wav | voice2json recognize-intent
```

[ha]: https://www.home-assistant.io/
[porcupine]: https://github.com/Picovoice/porcupine
[voice2json]: https://voice2json.org
