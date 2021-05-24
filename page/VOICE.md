# Voice I/O

## Output
espeak is preinstalled, and at best used with piping it to pulseaudio:
```
espeak --stdout "hello world" | paplay
```
Adjust the volume by turning the volume dial on the top.

## Input
For input there are two applications preinstalled:

* porcupine_demo_shm
* porcupine_send_shm

## porcupine_demo_shm
porcupine_demo_shm is really just a demo. It waits for the wake-word and says "hi" if it's detected.
You can run it like:
```
porcupine_demo_shm /usr/lib/libpv_porcupine.so /usr/share/porcupine/porcupine_params.pv /usr/share/porcupine/keyword_files/beaglebone/picovoice_beaglebone.ppn 0.5 < /tmp/spy &
/usr/local/bin/shmbuf_tool -m 2 -s 1 -S BMicsOut.shm -o /tmp/spy
```
Now say "picovoice" and receive a "hi" from the speaker.

## porcupine_send_shm
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

Hint: Other LED patterns can be found on the Echo in /usr/local/etc/ledd

I recommend to try it with [voice2json][voice2json]. After setting voice2json up according to the
voice2json documentation you can run this on the host:
```
nc -dNlp 13388 | cat voice/5s.wavheader - | voice2json transcribe-wav | voice2json recognize-intent
```

[voice2json]: https://voice2json.org
