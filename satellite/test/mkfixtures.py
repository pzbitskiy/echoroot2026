#!/usr/bin/env python3
"""Create the generated test inputs in fixtures/: button events and WAV files.

The four speech clips are not generated here. Put them in fixtures/audio as
16 kHz mono S16LE raw files, each with at least 0.5 s of silence first:
  time.raw   "Computer, what is the time?"
  time2.raw  the same sentence, ideally another voice
  alexa.raw  "Alexa, what is the weather today?"
  stop.raw   "Computer, stop."
"""
import math
import os
import struct
import wave

here = os.path.dirname(os.path.abspath(__file__))
fx = os.path.join(here, "fixtures")
os.makedirs(os.path.join(fx, "www"), exist_ok=True)
os.makedirs(os.path.join(fx, "audio"), exist_ok=True)


def event(etype, code, value):
    """struct input_event on 32-bit ARM, followed by EV_SYN."""
    return struct.pack("<IIHHi", 0, 0, etype, code, value) + struct.pack("<IIHHi", 0, 0, 0, 0, 0)


# Values as mapped by /usr/local/share/buttons/button_config.json.
for name, ev in {
    "uber_press": event(1, 138, 0), "uber_release": event(1, 138, 1),
    "mute_on": event(1, 113, 0), "mute_off": event(1, 113, 1),
    "vol_up": event(2, 0, -1), "vol_down": event(2, 0, 1),
}.items():
    with open(os.path.join(fx, name + ".bin"), "wb") as f:
        f.write(ev)


def tone(name, freq, secs, rate=48000, channels=1):
    with wave.open(os.path.join(fx, "www", name), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(
            struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / rate))) * channels
            for i in range(int(secs * rate))))


tone("tts.wav", 440, 1.5)
tone("chime.wav", 880, 0.4, rate=22050)
tone("announce.wav", 660, 1.0)
tone("media.wav", 330, 6.0, rate=44100, channels=2)
print("fixtures written to", fx)
