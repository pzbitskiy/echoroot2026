#!/usr/bin/env python3
"""Time speech-to-text servers that speak the Wyoming protocol.

Sends each clip the way Home Assistant does: audio in real time, then
audio-stop. Reports the time from audio-stop to the transcript, which is the
delay the speech-to-text stage adds to a voice command. A negative time means
the server found the end of speech itself and answered before audio-stop.

  stt-bench.py [-n RUNS] [--fast] NAME=HOST:PORT [NAME=HOST:PORT ...] -- CLIP ...

CLIP is a 16 kHz mono 16-bit WAV file, or a .raw file in that format.
--fast sends the audio without pacing (for servers that only work after
audio-stop the result is the same, and a run takes seconds instead of minutes).
Needs only the Python standard library.
"""
import argparse
import json
import os
import socket
import statistics
import sys
import threading
import time
import wave

RATE = 16000
CHUNK_MS = 30


def send(sock, etype, data=None, payload=b""):
    header = {"type": etype, "data": data or {}}
    if payload:
        header["payload_length"] = len(payload)
    sock.sendall(json.dumps(header).encode() + b"\n" + payload)


def recv(f):
    line = f.readline()
    if not line:
        raise ConnectionError("server closed the connection")
    header = json.loads(line)
    data = header.get("data") or {}
    if header.get("data_length"):
        data.update(json.loads(f.read(header["data_length"])))
    if header.get("payload_length"):
        f.read(header["payload_length"])
    return header["type"], data


def load(path):
    if path.endswith(".raw"):
        with open(path, "rb") as f:
            return f.read()
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (RATE, 1, 2):
            sys.exit("%s: need 16 kHz mono 16-bit" % path)
        return w.readframes(w.getnframes())


def describe(addr):
    with socket.create_connection(addr, timeout=10) as sock:
        f = sock.makefile("rb")
        send(sock, "describe")
        while True:
            etype, data = recv(f)
            if etype == "info":
                for asr in data.get("asr", []):
                    models = [m["name"] for m in asr.get("models", []) if m.get("installed", True)]
                    return "%s %s: %s" % (asr.get("name"), asr.get("version") or "", ", ".join(models))
                return "no speech-to-text service"


def transcribe(addr, pcm, fast):
    fmt = {"rate": RATE, "width": 2, "channels": 1}
    step = RATE * 2 * CHUNK_MS // 1000
    with socket.create_connection(addr, timeout=60) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        f = sock.makefile("rb")

        # A server that detects the end of speech itself (--vad-endpointing)
        # answers before audio-stop; the time is then negative.
        result = {}

        def read():
            try:
                while True:
                    etype, data = recv(f)
                    if etype == "transcript":
                        result["at"] = time.monotonic()
                        result["text"] = data.get("text", "")
                        return
            except (OSError, ValueError):
                pass

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        send(sock, "transcribe", {"language": "en"})
        send(sock, "audio-start", fmt)
        start = time.monotonic()
        for n, i in enumerate(range(0, len(pcm), step)):
            if not fast:
                delay = start + n * CHUNK_MS / 1000 - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
            send(sock, "audio-chunk", fmt, pcm[i:i + step])
        if not fast:
            delay = start + len(pcm) / (RATE * 2) - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        send(sock, "audio-stop")
        stop = time.monotonic()
        reader.join(60)
        if "text" not in result:
            raise TimeoutError("no transcript")
        return result["at"] - stop, result["text"]


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument("-n", type=int, default=5, help="timed runs per clip (default 5)")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("servers", nargs="+")
    ap.add_argument("clips", nargs="*")
    args, rest = ap.parse_known_args()
    names = [s for s in args.servers + rest if "=" in s]
    clips = [s for s in args.servers + args.clips + rest if "=" not in s and s != "--"]
    if not names or not clips:
        sys.exit(__doc__)
    servers = []
    for s in names:
        name, hp = s.split("=", 1)
        host, port = hp.rsplit(":", 1)
        servers.append((name, (host, int(port))))
    audio = [(os.path.basename(c), load(c)) for c in clips]

    for name, addr in servers:
        print("%s (%s:%d): %s" % (name, addr[0], addr[1], describe(addr)))
        transcribe(addr, audio[0][1], True)  # warm-up, not timed
    print()
    print("%-22s %6s  %s" % ("clip", "audio", "  ".join("%-9s" % n for n, _ in servers)))
    totals = {name: [] for name, _ in servers}
    texts = []
    for cname, pcm in audio:
        row = []
        for name, addr in servers:
            times = []
            for _ in range(args.n):
                t, text = transcribe(addr, pcm, args.fast)
                times.append(t)
            totals[name].append(statistics.median(times))
            row.append("%6.2f s " % statistics.median(times))
            texts.append((cname, name, text))
        print("%-22s %5.1fs  %s" % (cname, len(pcm) / (RATE * 2), "  ".join(row)))
    print("%-22s %6s  %s" % ("mean of medians", "", "  ".join(
        "%6.2f s " % statistics.mean(totals[n]) for n, _ in servers)))
    print()
    for cname, name, text in texts:
        print("%-22s %-10s %s" % (cname, name, text.strip()))


if __name__ == "__main__":
    main()
