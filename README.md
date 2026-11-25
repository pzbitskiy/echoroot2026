# echoroot

An Amazon Echo of the first generation (2016 or earlier) that runs without
Amazon: a local voice satellite for [Home Assistant](https://www.home-assistant.io/).
The wake word is recognised on the Echo, the LED ring behaves like the
original, and speech goes to and from a Home Assistant on your own network.

This repository continues the original echoroot project by André Hentschel,
<https://gitlab.com/AndreRH/echoroot> (project page:
<https://andrerh.gitlab.io/echoroot/>). The original built the hybrid system
that this work stands on and described the wiring. Added since 2026: a
reproducible build in Docker, the use of Amazon's own wake-word engine, the
Home Assistant satellite, installation to internal storage, and the speech
services on the Home Assistant side.

## AI disclaimer

SD card 1-bit mode, satellite service, documentation are Fable 5.2 - assisted.

## What it does

- "Alexa, what time is it", "Alexa, start a 4 minute timer", "Alexa, turn off
  the kitchen fan": answered by Home Assistant through the Echo's speaker.
  The wake word can also be "Computer", "Echo" or "Amazon".
- The ring shows listening, thinking, speaking, errors, a ringing timer and
  the muted microphones with the original animations.
- The mute button and the volume ring work as on the original. The action
  button starts a request without the wake word and stops an answer or a
  ringing timer.
- Home Assistant can make the Echo speak or play a sound (announcements,
  media player), sees and sets the microphone mute, and sees and stops a
  ringing timer.
- Speech-to-text, text-to-speech and an optional language model run on
  machines of your own.

## How it works

The system is a hybrid. Amazon's kernel (2.6.37), DSP firmware, wake-word
engine and its audio, LED and button daemons stay. A newer base system from
Buildroot replaces the rest, and one small C daemon, the satellite, replaces
the Alexa client.

```
mics -> ADCs -> audioproxyd -> DSP (beamforming + echo cancelling) -> /BMicsOut.shm
                                                          |
                              asrd (Amazon wake word) ----+---- echo-satellite
                                   | FoundKeyword              |  ESPHome native API
                                   +---------------------------+------> Home Assistant
                                                               |         STT -> intents/LLM -> TTS
speaker <- DSP <- /QSpeakerIn.shm <- PulseAudio lab126-sink <--+
LED ring <- ledd <- Pattern (D-Bus) <--------------------------+
buttons -> buttond -> button signal (D-Bus) ------------------->+
```

What the satellite reuses from Amazon's software:

| Function | Interface | Notes |
|---|---|---|
| Microphones | `/BMicsOut.shm` through `libshmbuffer.so` | 16 kHz mono, already beamformed and echo-cancelled by the DSP. A ring of about 65 s; the read position can be set, so streaming starts exactly where the wake word ended |
| Wake word | `asrd` with `libpryon.so`, D-Bus signals | Models for Alexa, Computer, Echo and Amazon. Runs with no other Amazon service and no network |
| Speaker | PulseAudio `lab126-sink` | Also the echo canceller's reference signal |
| LED ring | `ledd`, property `Pattern` | About 200 stock animations in `/usr/local/etc/ledd` |
| Buttons, volume ring | `buttond`, `audioCtrl` | Action button, mute (a hardware cut of the microphones), volume |

Why this shape:

- No open firmware exists for this Echo. Mainline Linux has a device tree for
  it but no microphone support and no driver for the DSP, and software
  beamforming is far too slow on its CPU. The DSP is only reachable on the
  stock kernel.
- Kernel 2.6.37 limits glibc to 2.23, so current Python, Go and Rust programs
  do not run. The satellite is therefore written in C and built with the
  Buildroot toolchain of this repository.
- Home Assistant's ESPHome integration gives a device timers, announcements
  and streamed answers. The satellite speaks that protocol, so Home Assistant
  needs nothing installed for it.

## Documentation

Getting an Echo running, in this order:

| Document | Content |
|---|---|
| [page/CIRCUIT.md](page/CIRCUIT.md) | What you need and how to wire the SD card and the serial port to the Echo's debug pads |
| [BUILD.md](BUILD.md) | Building `sdcard.img` in Docker, what the build depends on, how to add software to the image |
| [page/SETUP.md](page/SETUP.md) | Overview of the software: writing the card, login, WiFi, backup, the satellite's one setting, Bluetooth, MPD |
| [mmc/README.md](mmc/README.md) | Booting from the card in 1-bit mode, for wiring that cannot carry fast transfers |
| [INSTALL.md](INSTALL.md) | The whole sequence with its checks: satellite, Home Assistant, install to internal storage, a bootloader that starts without a serial cable |

Voice:

| Document | Content |
|---|---|
| [page/VOICE.md](page/VOICE.md) | Voice input and output on the Echo: the satellite, and the original Porcupine tools |
| [satellite/README.md](satellite/README.md) | The satellite daemon: configuration, behaviour, entities in Home Assistant, interfaces to Amazon's daemons, tests |
| [ASRD.md](ASRD.md) | Amazon's wake-word engine: how to run it alone and how it reports results |

Home Assistant side:

| Document | Content |
|---|---|
| [HA.md](HA.md) | How the satellite fits into Home Assistant: timers, announcements, streamed answers, commands without a language model, latency settings |
| [ha/README.md](ha/README.md) | Speech-to-text and text-to-speech services on your own machine, with measurements |
| [ha/LLM.md](ha/LLM.md) | Spoken questions answered by a local language model under llama.cpp, behind trigger words |
| [HA-ASSIST-DEBUG.md](HA-ASSIST-DEBUG.md) | Finding out what Home Assistant understood, without speaking to the Echo |

Background:

| Document | Content |
|---|---|
| [page/README.md](page/README.md) | The original author's account of how the project came about |

## Repository layout

| Path | Content |
|---|---|
| `rootfs/` | Fetches Amazon's firmware packages, builds the Buildroot base system and merges both |
| `u-boot/`, `x-load/` | Bootloaders (U-Boot is a submodule, X-Loader a prebuilt binary) |
| `sdcard/` | Assembles the card image |
| `mmc/` | Patch and script for the 1-bit card boot |
| `satellite/` | The Home Assistant satellite and its tests |
| `voice/` | The original Porcupine demos (Porcupine is a submodule) |
| `ha/` | Speech services, the trigger-word gate and service files for the Home Assistant side |
| `docker/` | The build container |
| `page/` | The original project page |

## Open points

- `asrd` is a closed binary from 2020 that links Amazon's download and
  metrics libraries. It opened no internet connection under emulation; this
  has not been confirmed on the device.
- The connection between Home Assistant and the Echo is not encrypted. The
  satellite restricts clients by address instead (`allow`).
- Amazon's package server is the one input of the build that cannot be
  replaced. Keep a copy of `rootfs/dl`; see BUILD.md.

## References

- Original project: <https://gitlab.com/AndreRH/echoroot>, <https://andrerh.gitlab.io/echoroot/>
- Clinton, Cook, Banik, "A Survey of Various Methods for Analyzing the Amazon
  Echo": <https://vanderpot.com/Clinton_Cook_Paper.pdf>
- echohacking wiki: <https://github.com/echohacking/wiki/wiki/Echo>
- Koyyle/EchoRooting, notes on lipc and D-Bus: <https://github.com/Koyyle/EchoRooting>
- echo-dot-assist, the same idea for the MediaTek-based Echos:
  <https://github.com/Gamer92000/echo-dot-assist>
- Mainline device tree: `arch/arm/boot/dts/ti/omap/omap3-echo.dts`

## Licence

GPL version 2, see [COPYING](COPYING). Amazon's firmware packages, which the
build downloads, and the Porcupine library and models keep their own terms
and are not part of this repository's licence.
