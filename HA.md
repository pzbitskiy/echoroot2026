# Home Assistant integration

How the Echo satellite fits into Home Assistant (HA): where the speech
services run, what the satellite has to speak, and what "timers",
"announcements" and "streaming TTS" mean on the HA side.

**Status:** the satellite side is implemented in `satellite/` (see
[satellite/README.md](satellite/README.md)), against ESPHome's `api.proto`
and HA's `esphome` integration, and running against Home Assistant.
The steps are in [INSTALL.md](INSTALL.md).
What was tried on the device and what only under emulation is listed in the
satellite's README.

## Overview

```
Echo (C satellite) <── ESPHome native API, TCP 6053 ──> Home Assistant VM
      │                                                   │
      │                                                   ├─ Wyoming, TCP ─> STT container
      │                                                   ├─ Wyoming, TCP ─> TTS container
      │                                                   └─ HTTP ─────────> LLM (optional)
      └── HTTP GET of TTS / announcement audio ──────────>┘
```

- HA owns the conversation. It runs an **Assist pipeline**: speech-to-text,
  then a conversation agent (sentence matching, optionally an LLM), then
  text-to-speech.
- The Echo is an **Assist satellite**: a device with a microphone, a speaker
  and its own wake word. It streams audio to HA after the wake word and plays
  what HA sends back.
- STT, TTS and the LLM are separate network services. They do not have to run
  inside the HA VM.

## Where STT, TTS and the LLM run

HA reaches speech engines through its **Wyoming** integration, which is a
plain TCP client. An "add-on" in HA OS is only a container that HA's
supervisor manages; the same servers run as ordinary containers anywhere HA
can reach.

Recommended layout: one container host or VM next to HA, with a GPU if you
have one.

| Service | Typical port | Notes |
|---|---|---|
| STT: `rhasspy/wyoming-whisper` | 10300 | The official server. For English it uses Parakeet by default (0.3 s after end of speech in our measurement, against 1.2 s for Whisper `base-int8`); Whisper for most other languages. About 1.1 GB RAM with Parakeet |
| TTS: Piper (`rhasspy/wyoming-piper`) | 10200 | Supports streaming synthesis |
| LLM: llama.cpp (`llama-server`) or Ollama | 8080, 11434 | Optional; plain HTTP, not Wyoming. Setup with llama.cpp and trigger words: [ha/LLM.md](ha/LLM.md) |

A tested compose file for the two speech services, the measurements and the
script that made them are in [ha/](ha/README.md). Home Assistant's own Whisper
app is the same server, so on its defaults it already runs Parakeet for
English.

In HA: Settings → Devices & services → Add integration → **Wyoming Protocol**,
once per service, with the container host's address and port. For the LLM, add
the **llama.cpp** or **Ollama** integration with its URL. Then create a pipeline under Settings
→ Voice assistants and pick the STT engine, the conversation agent and the TTS
voice.

Audio goes Echo → HA → STT container. The second hop is on the LAN or the same
hypervisor and adds no meaningful latency.

## The satellite side

### Why the ESPHome native API

HA knows three ways to talk to a voice device:

| | Timers | Announcements | Streaming TTS | Status |
|---|---|---|---|---|
| ESPHome native API | yes | yes | yes | What HA's own voice hardware uses; actively developed |
| Wyoming satellite | yes | yes | yes | Simpler protocol, but the reference satellite was archived in January 2026 and its maintainers call that side deprecated |
| Websocket `assist_pipeline/run` | no | no | yes | No device entity in HA; prototype only |

With the first two, HA creates an `assist_satellite` entity for the device
and has dedicated protocol messages for the features below. With the
websocket API, HA only runs one STT → intent → TTS pass for whoever is
connected, and has nothing to address afterwards.

The cost of ESPHome is that the satellite implements a protobuf protocol that
changes fairly often. Wyoming would be the fallback; the satellite does not
speak it. `Gamer92000/echo-dot-assist`, the same idea for the MediaTek-based
Echos, speaks both.

### Connection

- HA connects **to the device**, TCP port 6053. The device announces itself
  with mDNS (`_esphomelib._tcp`), or is added in HA by IP address.
- Messages are length-prefixed protobuf, optionally wrapped in Noise
  encryption with a pre-shared key. The satellite does not implement the
  encryption: it restricts clients by address instead (`allow`).
- On connect the device describes itself. For voice it sets feature flags:
  voice assistant, API audio, timers, announce, start conversation. HA only
  uses a feature the device has declared.
- The connection stays open. Keeping it open is also what makes the start of
  a request fast.

The image already has avahi for mDNS and libcurl for HTTP. The protobuf
encoding is hand-written in `satellite/pb.c`.

### Entities

Besides the `assist_satellite` entity, which HA creates itself, the satellite
declares:

| Entity | Type | Purpose |
|---|---|---|
| Media player | `media_player` | Volume, speaker mute, `media_player.play_media`, announcements. It also tells HA the audio format to convert to (WAV, mono, 16 bit, 48 kHz) |
| Mute | `switch` | The microphones, as with the mute button. Shows the button's state and can be switched from HA |
| Timer ringing | `binary_sensor` | On while a finished timer rings |
| Stop ringing | `button` | Silences a ringing timer |

For example, to stop a ringing timer when someone leaves the kitchen, or to
mute the microphones at night:

```yaml
- action: button.press
  target:
    entity_id: button.echo_stop_ringing
- action: switch.turn_on
  target:
    entity_id: switch.echo_mute
```

The entity names depend on the name the device has in HA.

### One voice request, step by step

| Step | Who | What | LED ring |
|---|---|---|---|
| 1 | `asrd` | Wake word found, `FoundKeyword` signal with buffer position | `active_start` |
| 2 | Satellite → HA | "Start a pipeline run", flagged as wake word already detected, with the wake word name | |
| 3 | Satellite → HA | 16 kHz mono 16-bit PCM in small chunks, starting at the end of the wake word | `alexa_user-talking` |
| 4 | HA → Satellite | Event: speech ended (HA's voice activity detection decides) | `alexa_thinking` |
| 5 | HA | STT, then intent matching or LLM | |
| 6 | HA → Satellite | Event: STT result text; event: intent done | |
| 7 | HA → Satellite | Event: TTS available, with a URL on HA | |
| 8 | Satellite | Fetch the URL, play through `lab126-sink` | `dialog-talk` |
| 9 | Satellite → HA | "Playback finished" | `off` |
| 10 | HA → Satellite | Event: run ended, or error | `error` on failure |

The events HA sends map one to one onto LED states, which is how the ring can
behave like the original firmware.

Two ways exist for step 7 and 8:

- **Device fetches a URL.** HA transcodes to a format the device declared.
  This is the path that supports streaming TTS and announcements. Chosen here;
  the device declares WAV so the Cortex-A8 does not spend time decoding.
- **HA pushes PCM over the API connection** at 16 kHz. Simpler, no HTTP
  client, but it starts only after synthesis has finished and announcements
  still need the URL path.

## Timers

- These are HA's voice timers: "start a 4 minute timer", "how long is left",
  "add a minute", "cancel the timer". They are not `timer` helper entities.
- Built-in intents: `HassStartTimer`, `HassCancelTimer`, `HassIncreaseTimer`,
  `HassDecreaseTimer`, `HassPauseTimer`, `HassUnpauseTimer`,
  `HassTimerStatus`.
- HA keeps the timer and ties it to the device that asked. If that device has
  not declared timer support, HA answers that the device cannot set timers.
  This is the main reason the websocket API is not enough.
- HA sends the device timer events: started, updated, cancelled, finished,
  each with an id, the name, total seconds and seconds left.
- Everything the user sees and hears is the device's job:
  - ring when it finishes (the image has the stock sounds under
    `/usr/local/share/earcon`), with `ready_timer` on the ring,
  - stop ringing on the action button, on "computer, stop" (which `asrd`
    recognises locally as `FoundCmd STOP` without a server round trip) or on
    the "Stop ringing" button entity.
- The satellite shows nothing while a timer runs; only the ringing is
  implemented. "Timer ringing" tells HA that it rings.

## Announcements

HA can make the satellite speak without being asked. This is what scripted
automations use.

```yaml
action: assist_satellite.announce
target:
  entity_id: assist_satellite.echo
data:
  message: "The washing machine is done."
```

- HA synthesises the text, sends the device an announce request with the audio
  URL (optionally a chime to play first), and waits for "announcement
  finished".
- `assist_satellite.start_conversation` does the same and then opens the
  microphone for a reply without a wake word: "The garage is still open.
  Should I close it?"
- `assist_satellite.ask_question` waits for an answer and returns it to the
  automation.
- On the device an announcement is the playback half of a voice request:
  fetch, play with `dialog-talk` on the ring, report finished. An
  announcement interrupts a request in progress; a wake word interrupts an
  announcement.

## Streaming TTS

- **Without it:** HA waits for the complete response text, synthesises the
  whole clip, then tells the device to fetch it.
- **With it:** HA gives the device the audio URL early, the TTS engine
  produces audio while the text is still being generated, and the device plays
  the HTTP stream as it arrives.
- The gain is for LLM answers. HA's own figures are about 0.5 s to first audio
  instead of more than 5 s for long replies. For "what is the time" there is
  little to save.
- Requirements: a TTS engine that streams (Piper does), an LLM conversation
  agent that streams text, and a device that plays a progressive HTTP stream.
  The satellite therefore plays while downloading instead of downloading
  first.

## Commands without an LLM

HA matches sentences locally before anything is sent to an LLM, if the
pipeline's "Prefer handling commands locally" option is on (or if no LLM is
configured at all).

- **Time and date:** `HassGetCurrentTime`, `HassGetCurrentDate`. No setup.
- **Timers:** as above.
- **Weather:** `HassGetWeather`. Needs a weather entity exposed to Assist.
- **Devices:** lights, switches, covers, climate and so on, for every entity
  exposed to Assist.
- **Your own phrases**, two ways:
  - an automation with a sentence trigger:

    ```yaml
    triggers:
      - trigger: conversation
        command: "movie time"
    actions:
      - action: scene.turn_on
        target: {entity_id: scene.movie}
      - set_conversation_response: "Enjoy the film."
    ```
  - `custom_sentences/en/*.yaml` together with `intent_script`, for phrases
    with slots and lists.

One forum report describes the local-first option making responses much slower
in a particular setup, so measure with it on and off.

## Latency settings on the HA side

In rough order of effect:

1. **End-of-speech detection.** HA waits for 0.7 s of silence by default
   before it stops listening. Each satellite device gets a "Finished speaking
   detection" setting; "aggressive" is 0.25 s, "relaxed" 1.25 s. This is
   usually the largest fixed delay.
2. **STT engine.** Parakeet or a small Whisper model; see the table above.
3. **Local intents first**, so common commands never reach the LLM.
4. **Streaming TTS**, for LLM answers.
5. **Keep everything on the LAN** and the satellite's connection open.

Estimate for a local-intent command: 1 to 1.5 s from end of speech to first
audio. Not measured.

## Open points

- Noise encryption on the API connection is not implemented. It would cost
  CPU on the Echo; `allow` and a trusted LAN stand in for it.
- The latency estimate above was not measured stage by stage. Field testing showed it is acceptable.

See also the [README](README.md) for the whole system and [ASRD.md](ASRD.md)
for the wake-word side.
