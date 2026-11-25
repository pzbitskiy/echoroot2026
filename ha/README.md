# Speech services for Home Assistant

Speech-to-text and Piper text-to-speech as two containers on a
machine of your own, used by Home Assistant over the Wyoming protocol. The
Echo is not involved: it talks only to Home Assistant, which picks the
engines (see [../HA.md](../HA.md)).

A language model for spoken questions, with llama.cpp, is in [LLM.md](LLM.md).

## What Home Assistant's own Whisper app runs

The "Whisper" app that Home Assistant's voice wizard installs is the same
server as the `rhasspy/wyoming-whisper` image used here
(`wyoming-faster-whisper` 3.9.1 in app version 3.6.1). Despite its name it is
no longer Whisper-only. With its default settings (`stt_library: auto`,
`language: en`) it uses **Parakeet for English**; Whisper `base-int8` is what
it uses for most other languages, and what older versions used for English.

So a current app on defaults already runs Parakeet, and moving it to another
machine changes where it runs, not the model. To see which model yours uses:
Settings → Apps → Whisper → Log. A line with `sherpa-onnx-nemo-parakeet`
means Parakeet. Or measure it, see "Comparing with Home Assistant's app".

## Install

Needs Docker with the compose plugin, about 3 GB of disk and 1.5 GB of RAM.

```sh
mkdir ~/voice && cd ~/voice
# copy docker-compose.yml from this folder here, then:
docker compose up -d
docker compose logs -f        # wait for "Ready" twice; first start downloads the models
```

First start downloads about 0.8 GB (the Qwen3-ASR model) and 60 MB (the Piper voice) into
`stt-data/` and `tts-data/`. After that the services start without internet.

In Home Assistant:

1. Settings → Devices & services → Add integration → **Wyoming Protocol**:
   host = the machine's address, port `10300`. Repeat with port `10200`.
2. Settings → Voice assistants → Add assistant (or edit one): choose
   `faster-whisper` as speech-to-text and `piper` as text-to-speech.
3. On the Echo's device page, select that assistant.

Home Assistant's own Whisper and Piper apps announce themselves under the same
two names, and the assistant's settings then list two entries called
`faster-whisper`: the name comes from the server, not from the title of the
integration entry. Rename the entity to tell them apart (Settings → Devices &
services → Entities, search `faster_whisper`), or remove the apps and their
Wyoming entries so only one of each is left. The renaming is from Home
Assistant's source and was not confirmed on a running system.

If Home Assistant's own Whisper app logs that the CPU lacks AVX while the
processor has it, and Home Assistant runs as a Proxmox VM: the CPU type
`x86-64-v2-AES` hides AVX and AVX2. Set the type to `host` or `x86-64-v3` and
stop and start the VM. Containers (LXC) use the host's CPU as it is.

### Without Docker

The server is a Python package and is itself the Wyoming service: nothing
else has to run next to it. Tried in clean Debian 12 (Python 3.11) and
Debian 13 (Python 3.13) systems, and in use in an LXC container with the
service file from this folder. As root:

```sh
apt install python3 python3-venv
python3 -m venv /opt/wyoming-stt
/opt/wyoming-stt/bin/pip install 'wyoming-faster-whisper[qwen3-asr]==3.9.1'

useradd --system --home /var/lib/wyoming-stt --shell /usr/sbin/nologin wyoming
mkdir -p /var/lib/wyoming-stt && chown wyoming: /var/lib/wyoming-stt

cp wyoming-stt.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now wyoming-stt
journalctl -u wyoming-stt -f      # wait for "Ready"; first start downloads 0.75 GB
```

Disk: 0.45 GB for the program, 0.75 GB for the model. Memory: about 1.5 GB.
For Parakeet instead, install the extra `sherpa` in place of `qwen3-asr` and
drop `--stt-library qwen3-asr --vad-clip` from the service file (not tried
outside Docker).

## Latency

Time from the end of the audio to the transcript, which is what the
speech-to-text stage adds to a command. Eight commands of 1 to 3 s spoken by
Piper, each with 0.5 s of silence before and 0.8 s after, sent in real time as
Home Assistant does; median of 3 runs per clip, mean over the clips. Measured
with `stt-bench.py`.

| Engine | 4 threads (default) | 8 threads | Memory |
|---|---|---|---|
| Parakeet TDT 0.6b v2 int8 (default for English) | 0.31 s | 0.25 s | 1.1 GB |
| Whisper `base-int8` (`--stt-library faster-whisper --model base-int8`) | 1.20 s | 1.02 s | 0.5 GB |
| Whisper `small-int8` | | 2.09 s | 0.5 GB |
| Streaming zipformer (`--stt-library sherpa --sherpa-streaming`) | | 0.10 s | 0.2 GB |

- Parakeet is about four times faster than Whisper `base-int8` here, and its
  time grows with the length of the audio (0.19 s for a 2.3 s clip, 0.37 s
  for 4.3 s) while Whisper costs about a second whatever the length.
- All four transcribed all eight clips correctly, so these clips say nothing
  about accuracy: synthetic speech without noise is the easy case. The
  streaming model is described by the app's documentation as faster but less
  accurate.
- Piper produced each of these sentences in 0.05 to 0.25 s.
- On a second machine Qwen3-ASR with `--vad-endpointing 0.5 --vad-clip --cpu-threads 8` answered
  0.18 s after the end of the audio on average and had all eight clips
  right. The other engines were not measured there.

For scale: Home Assistant waits for 0.7 s of silence by default before it
ends a command (0.25 s with "aggressive" finished-speaking detection on the
device), so with Parakeet that wait is the larger part of the delay.

## Accuracy on recordings from the Echo

Twenty commands recorded by Home Assistant from the Echo (`assist_pipeline:
debug_recording_dir`), one speaker. Four of them, spoken a step further away and before the satellite had
its microphone gain, were understood by no engine and are left out below.

| Engine | Correct of 16 | After end of audio | Memory |
|---|---|---|---|
| Parakeet TDT 0.6b v2 (default for English) | 10 | 0.28 s | 0.9 GB |
| Qwen3-ASR 0.6B (`--stt-library qwen3-asr`) | 15 | 0.94 s | 1.6 GB |
| Qwen3-ASR with `--vad-clip --cpu-threads 8` | 14 | 0.70 s | |
| Qwen3-ASR given the device names as `--initial-prompt` | 14 | 0.92 s | 1.2 GB |
| Canary 1B v2 (`--stt-library onnx-asr --model istupakov/canary-1b-v2-onnx`) | 10 | 0.87 s | 4.7 GB |
| Whisper `small.en` with the device names as prompt | 10 | 3.5 s | 1.4 GB |

- What was said is known for certain for one clip and inferred for the rest
  from what the engines agree on.
- The typical Parakeet errors were "to own" for "turn on" and "pan" or "hand"
  for "fan". Small changes to a clip (0.1 s trimmed from the start) flipped
  such words, with every engine but least with Qwen3-ASR.
- Device names as a prompt did not help Qwen3-ASR and on weak audio it
  returned the prompt itself. The server's `--hass-token` option builds such
  a prompt from Home Assistant; not tried.
- With `--vad-endpointing 0.5` Qwen3-ASR answered 0.13 s after the point where
  Home Assistant's 0.7 s detection ended each recording, which is earlier
  than Parakeet without the option (0.3 s after it). Parakeet with the option:
  0.31 s before that point.

## End-of-speech time

Home Assistant decides when a command has ended, after 0.25, 0.7 (default) or
1.25 s of silence ("Finished speaking detection" on the device page); there is
no value in between. The server can take that over with
`--vad-endpointing <seconds>`: it then reports that it needs no external
endpointing and answers as soon as it has heard that much silence. The option
is not among the settings of Home Assistant's Whisper app, so it needs the
container.

Tried with `--vad-endpointing 0.5` on five commands recorded by Home Assistant
from the Echo (which end where Home Assistant's 0.7 s detection stopped
them): the transcript was ready 0.26 s before the end of the recording on
average, against 0.32 s after it without the option, and the text was the
same in 9 of 10 runs. With Home Assistant itself commands end and are
answered with the option on (about 2.5 s from the wake word to the first
audio); whether the server's 0.5 s or Home Assistant's own detection ended them was not measured. Home Assistant's
Wyoming integration reads the server's flag in the current development
source.

## Measuring on your own machine

`stt-bench.py` needs only Python 3. On the machine running the services:

```sh
python3 stt-bench.py parakeet=127.0.0.1:10300 -- clips/*.wav
```

To compare with Whisper on the same hardware, start a second server next to
it and name both:

```sh
docker run -d --name whisper-base -p 10310:10300 -v $PWD/stt-data:/data \
    rhasspy/wyoming-whisper:3.9.1 --language en --stt-library faster-whisper --model base-int8
python3 stt-bench.py parakeet=127.0.0.1:10300 whisper=127.0.0.1:10310 -- clips/*.wav
docker rm -f whisper-base
```

Your own recordings are a better test than the supplied clips: 16 kHz mono
16-bit WAV.

### Comparing with Home Assistant's app

The Whisper app does not publish its port by default. In Settings → Apps →
Whisper → Configuration → Network, enter `10300` and restart the app, then:

```sh
python3 stt-bench.py vm=127.0.0.1:10300 ha=<home assistant address>:10300 -- clips/*.wav
```

If both columns show about the same time, the app already runs Parakeet.
