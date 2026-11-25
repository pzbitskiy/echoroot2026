"""Stand-in for Home Assistant: drives echo-satellite through aioesphomeapi.

Reacts to the device like HA's esphome integration does, and to command
files (out/cmd.<name>) written by the test script inside the chroot.
"""
import asyncio
import os
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from aioesphomeapi import APIClient
from aioesphomeapi.model import (
    MediaPlayerCommand,
    VoiceAssistantEventType as E,
    VoiceAssistantTimerEventType as T,
)

OUT = sys.argv[1]
WWW = sys.argv[2]
HTTP_PORT = 8123
T0 = time.monotonic()
log_file = open(os.path.join(OUT, "ha.log"), "w", buffering=1)


def log(*a):
    print(f"{time.monotonic() - T0:8.3f}", *a, file=log_file)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log("http:", fmt % args)

    def do_GET(self):
        if self.path == "/stream.wav":
            # Streaming TTS: no length, data size unknown, audio trickles in.
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.end_headers()
            rate = 48000
            hdr = b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt " + struct.pack(
                "<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", 0xFFFFFFFF)
            self.wfile.write(hdr[:20])          # header split across writes
            self.wfile.flush()
            time.sleep(0.05)
            self.wfile.write(hdr[20:])
            import math
            for chunk in range(10):             # 2 s of audio in 10 odd-sized pieces
                n = rate // 5
                pcm = b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 550 * i / rate)))
                               for i in range(chunk * n, (chunk + 1) * n))
                self.wfile.write(pcm[:4001])
                self.wfile.flush()
                time.sleep(0.1)
                self.wfile.write(pcm[4001:])
                self.wfile.flush()
            return
        path = os.path.join(WWW, os.path.basename(self.path))
        if not os.path.isfile(path):
            self.send_error(404)
            return
        data = open(path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def url(name):
    return f"http://127.0.0.1:{HTTP_PORT}/{name}"


class FakeHA:
    def __init__(self):
        self.cli = None
        self.run = 0
        self.audio = None
        self.audio_bytes = 0
        self.first_audio = None
        self.pipeline_task = None
        self.streaming = False     # next run answers with streaming TTS
        self.silent = False        # next run recognises nothing
        self.keys = {}             # entity type name -> key

    async def connect(self):
        self.cli = APIClient("127.0.0.1", 6053, None)
        while True:
            try:
                await self.cli.connect(login=True)
                break
            except Exception as err:  # satellite not up yet
                await asyncio.sleep(0.3)
                self.last_err = err
        info = await self.cli.device_info()
        log("device_info:", info.name, info.mac_address, info.model, "features", info.voice_assistant_feature_flags,
            "api", self.cli.api_version)
        entities, _ = await self.cli.list_entities_services()
        for e in entities:
            log("entity:", type(e).__name__, e.key, e.name, getattr(e, "supported_formats", None),
                getattr(e, "feature_flags", None))
            self.keys[type(e).__name__] = e.key
        self.cli.subscribe_states(lambda s: log("state:", s))
        self.cli.subscribe_voice_assistant(
            handle_start=self.handle_start,
            handle_stop=self.handle_stop,
            handle_audio=self.handle_audio,
            handle_announcement_finished=self.handle_announcement_finished,
        )
        cfg = await self.cli.get_voice_assistant_configuration(5)
        log("va config:", [(w.id, w.wake_word) for w in cfg.available_wake_words], cfg.active_wake_words,
            cfg.max_active_wake_words)
        open(os.path.join(OUT, "ha.connected"), "w").close()

    def ev(self, event, data=None):
        log("-> event", event.name, data or "")
        self.cli.send_voice_assistant_event(event, data or {})

    async def handle_start(self, conversation_id, flags, audio_settings, wake_word_phrase):
        self.run += 1
        log(f"handle_start run={self.run} conversation_id={conversation_id!r} flags={flags} "
            f"wake_word_phrase={wake_word_phrase!r} audio_settings={audio_settings}")
        if self.audio:
            self.audio.close()
        self.audio = open(os.path.join(OUT, f"ha_audio_{self.run}.raw"), "wb", buffering=0)
        self.audio_bytes = 0
        self.first_audio = None
        if self.pipeline_task:
            self.pipeline_task.cancel()
        self.pipeline_task = asyncio.create_task(self.pipeline(self.run))
        return 0

    async def handle_stop(self, abort):
        log(f"handle_stop abort={abort}")
        if abort and self.pipeline_task:
            self.pipeline_task.cancel()
            self.pipeline_task = None

    async def handle_audio(self, data, data2=None):
        if self.first_audio is None:
            self.first_audio = time.monotonic()
            log(f"first audio chunk: {len(data)} bytes")
        self.audio_bytes += len(data)
        if self.audio:
            self.audio.write(data)

    async def handle_announcement_finished(self, msg):
        log("announcement finished:", msg)

    async def pipeline(self, run):
        streaming, silent = self.streaming, self.silent
        self.streaming = self.silent = False
        try:
            self.ev(E.VOICE_ASSISTANT_RUN_START, {"url": url("stream.wav")} if streaming else None)
            self.ev(E.VOICE_ASSISTANT_STT_START)
            # "End of speech" once 1.6 s of command audio arrived.
            while self.audio_bytes < int(1.6 * 32000):
                await asyncio.sleep(0.02)
            log(f"got {self.audio_bytes} bytes {time.monotonic() - self.first_audio:.3f} s after the first chunk")
            self.ev(E.VOICE_ASSISTANT_STT_VAD_END)
            await asyncio.sleep(0.2)
            if silent:
                self.ev(E.VOICE_ASSISTANT_ERROR, {"code": "stt-no-text-recognized", "message": "No text recognized"})
                self.ev(E.VOICE_ASSISTANT_RUN_END)
                return
            self.ev(E.VOICE_ASSISTANT_STT_END, {"text": "what is the time"})
            self.ev(E.VOICE_ASSISTANT_INTENT_START)
            if streaming:
                await asyncio.sleep(0.1)
                self.ev(E.VOICE_ASSISTANT_INTENT_PROGRESS, {"tts_start_streaming": "1"})
                await asyncio.sleep(1.0)
            self.ev(E.VOICE_ASSISTANT_INTENT_END, {"conversation_id": f"conv-{run}", "continue_conversation": "0",
                                                   "speech": "It is half past one."})
            self.ev(E.VOICE_ASSISTANT_TTS_START, {"text": "It is half past one."})
            await asyncio.sleep(0.1)
            self.ev(E.VOICE_ASSISTANT_TTS_END, {"url": url("stream.wav" if streaming else "tts.wav")})
            self.ev(E.VOICE_ASSISTANT_RUN_END)
        except asyncio.CancelledError:
            log(f"pipeline {run} cancelled")

    async def command(self, name):
        log("command:", name)
        if name == "timer":
            self.cli.send_voice_assistant_timer_event(T.VOICE_ASSISTANT_TIMER_STARTED, "t1", "tea", 240, 240, True)
            await asyncio.sleep(0.5)
            self.cli.send_voice_assistant_timer_event(T.VOICE_ASSISTANT_TIMER_FINISHED, "t1", "tea", 240, 0, True)
        elif name == "announce":
            r = await self.cli.send_voice_assistant_announcement_await_response(
                url("announce.wav"), 20, "The washing machine is done.", preannounce_media_id=url("chime.wav"))
            log("announce returned:", r)
        elif name == "announce_bad":
            r = await self.cli.send_voice_assistant_announcement_await_response(url("missing.wav"), 20, "Lost.")
            log("announce_bad returned:", r)
        elif name == "ask":
            r = await self.cli.send_voice_assistant_announcement_await_response(
                url("announce.wav"), 20, "Should I close the garage?", start_conversation=True)
            log("ask returned:", r)
        elif name == "volume":
            self.cli.media_player_command(self.keys["MediaPlayerInfo"], volume=0.25)
        elif name == "media":
            self.cli.media_player_command(self.keys["MediaPlayerInfo"], media_url=url("media.wav"))
        elif name == "media_stop":
            self.cli.media_player_command(self.keys["MediaPlayerInfo"], command=MediaPlayerCommand.STOP)
        elif name in ("mute_on", "mute_off"):
            self.cli.switch_command(self.keys["SwitchInfo"], name == "mute_on")
        elif name == "stop_ringing":
            self.cli.button_command(self.keys["ButtonInfo"])
        elif name == "ww_alexa":
            await self.cli.set_voice_assistant_configuration(active_wake_words=["alexa"])
            cfg = await self.cli.get_voice_assistant_configuration(5)
            log("va config now:", cfg.active_wake_words)
        elif name == "ww_computer":
            await self.cli.set_voice_assistant_configuration(active_wake_words=["computer"])
        elif name == "drop":
            await self.cli.disconnect(force=True)
            log("dropped the connection")
            await asyncio.sleep(2)
            await self.connect()
            log("reconnected")
        elif name == "streaming":
            self.streaming = True
        elif name == "silent":
            self.silent = True

    async def main(self):
        await self.connect()
        while True:
            for f in sorted(os.listdir(OUT)):
                if f.startswith("cmd."):
                    os.unlink(os.path.join(OUT, f))
                    asyncio.create_task(self.command(f[4:]))
            await asyncio.sleep(0.1)


threading.Thread(target=ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler).serve_forever, daemon=True).start()
log("fake HA started")
try:
    asyncio.run(FakeHA().main())
except Exception as err:
    log("FATAL", repr(err))
    raise
