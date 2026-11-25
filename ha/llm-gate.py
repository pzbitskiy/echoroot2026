#!/usr/bin/env python3
"""Trigger-word gate in front of llama-server, for Home Assistant.

Home Assistant sends every sentence its own matcher did not understand to
the conversation agent of the voice assistant. This program sits between
Home Assistant and llama-server (or any OpenAI-compatible server) and lets a
sentence through only if it starts with one of the trigger words; for
everything else it answers "Sorry, I couldn't understand that" itself, at
once, without touching the model. See LLM.md.

    llm-gate.py --listen 0.0.0.0:8090 --backend http://127.0.0.1:8080 \
                --keep explain --strip question,llm

--keep words stay in the text given to the model ("explain why ..."),
--strip words are removed ("question, how far ..." becomes "how far ...").
An answer to a question the model asked goes through without a trigger word.

It also takes the time of day out of the system prompt. Home Assistant
writes "Current time is 12:34:56." there, so the prompt differs on every
request, and models that can reuse their prompt cache only from a checkpoint
(Qwen3.5 is one) then read the whole prompt again each time: 4 s instead of
0.7 s to the first word on the machine this was tried on. The date stays.
--keep-time leaves the prompt alone.

Everything but POST .../chat/completions is forwarded unchanged. Needs only
Python 3.9 or newer.
"""

from __future__ import annotations

import argparse
import hmac
import http.client
import json
import re
import sys
import time
import urllib.parse
from collections.abc import Iterable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

Message = dict[str, Any]                # one chat message of the OpenAI API

QUESTION_MARKS = ("?", ";", "？")       # what Home Assistant treats as a question
# The time Home Assistant appends to its instructions (helpers/llm.py).
TIME_OF_DAY = re.compile(r"Current time is \d{1,2}:\d{2}:\d{2}\. ?")


def text_of(message: Message) -> str:
    """The text of a chat message, whose content is a string or a list of parts."""
    content = message.get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return " ".join(p.get("text", "") for p in content
                        if isinstance(p, dict) and p.get("type") == "text")
    return ""


def with_text(message: Message, text: str) -> Message:
    """A copy of the message with its text replaced."""
    message = dict(message)
    content = message.get("content")
    if isinstance(content, list):
        parts: list[Any] = []
        done = False
        for p in content:
            if isinstance(p, dict) and p.get("type") == "text":
                if not done:
                    parts.append({**p, "text": text})
                    done = True
            else:
                parts.append(p)
        message["content"] = parts
    else:
        message["content"] = text
    return message


class Gate:
    def __init__(self, keep: Iterable[str], strip: Iterable[str], reply: str) -> None:
        self.keep: set[str] = {w.lower() for w in keep}
        self.strip: set[str] = {w.lower() for w in strip}
        self.reply: str = reply
        words = sorted(self.keep | self.strip, key=len, reverse=True)
        # The word at the start, whatever punctuation the speech-to-text
        # engine put around it: "Explain, ...", "Question: ...", "LLM. ...".
        # A word that is spelled when spoken arrives spelled as well: Qwen3-ASR
        # wrote "LLM", "L L M" and "L. L. M." for the same word.
        self.words: list[str] = words
        self.pattern: re.Pattern[str] | None = re.compile(
            r"^[\W_]*(?:%s)(?![\w'])[\s,.:;!?\-–—]*" % "|".join(
                "(?P<w%d>%s|%s)" % (n, re.escape(w), r"[\s.\-]+".join(map(re.escape, w)))
                for n, w in enumerate(words)),
            re.IGNORECASE) if words else None

    def match(self, text: str) -> tuple[bool, str]:
        """(triggered, text for the model)."""
        if self.pattern is None:
            return True, text               # no words configured: gate is open
        m = self.pattern.match(text)
        if not m:
            return False, text
        group = m.lastgroup                 # "w<n>": every word has its own group
        assert group is not None
        if self.words[int(group[1:])] in self.strip:
            rest = text[m.end():].strip()
            return bool(rest), rest[:1].upper() + rest[1:]
        return True, text.strip()

    def canned(self, message: Message) -> bool:
        return message.get("role") == "assistant" and text_of(message).strip() == self.reply

    def apply(self, messages: list[Message]) -> tuple[list[Message], bool]:
        """(messages for the model, whether to ask the model at all).

        Earlier turns are rewritten the same way every time, so that the
        model's prompt cache stays valid: refused sentences and their canned
        answers are dropped, stripped words stay stripped.
        """
        if not messages or messages[-1].get("role") != "user":
            return messages, True           # tool results and the like
        last = len(messages) - 1
        triggered, text = self.match(text_of(messages[last]))
        follow_up = (last > 0 and messages[last - 1].get("role") == "assistant"
                     and not self.canned(messages[last - 1])
                     and text_of(messages[last - 1]).strip().endswith(QUESTION_MARKS))
        if not (triggered or follow_up):
            return messages, False

        out: list[Message] = []
        i = 0
        while i < last:
            m = messages[i]
            if m.get("role") == "user":
                if i + 1 < last and self.canned(messages[i + 1]):
                    i += 2
                    continue
                hit, rest = self.match(text_of(m))
                if hit:
                    m = with_text(m, rest)
            out.append(m)
            i += 1
        out.append(with_text(messages[last], text) if triggered else messages[last])
        return out, True


def without_time(messages: list[Message]) -> list[Message]:
    """The messages with the time of day removed from the system prompt."""
    return [with_text(m, TIME_OF_DAY.sub("", text_of(m)))
            if m.get("role") in ("system", "developer") and isinstance(m.get("content"), str)
            else m for m in messages]


def canned_completion(model: str, reply: str, stream: bool) -> bytes:
    """The gate's own answer, in the form llama-server would have sent it."""
    base: dict[str, Any] = {"id": "chatcmpl-gate-%d" % time.time_ns(),
                            "created": int(time.time()), "model": model}
    if not stream:
        return json.dumps({
            **base, "object": "chat.completion",
            "choices": [{"index": 0, "finish_reason": "stop",
                         "message": {"role": "assistant", "content": reply}}],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        }).encode()
    deltas: list[tuple[dict[str, str], str | None]] = [
        ({"role": "assistant", "content": ""}, None), ({"content": reply}, None), ({}, "stop")]
    chunks: list[str] = []
    for delta, finish in deltas:
        chunks.append("data: " + json.dumps({
            **base, "object": "chat.completion.chunk",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}))
    chunks.append("data: [DONE]")
    return ("\n\n".join(chunks) + "\n\n").encode()


class GateServer(ThreadingHTTPServer):
    """The HTTP server, with the settings its request handlers use."""
    daemon_threads = True
    gate: Gate
    backend: urllib.parse.SplitResult
    backend_host: str
    timeout_s: float
    verbose: bool
    keep_time: bool
    max_body: int
    keys: list[str]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "llm-gate"
    server: GateServer      # pyright: ignore[reportIncompatibleVariableOverride]

    def log_message(self, format: str, *args: Any) -> None:     # no access log; see log()
        pass

    def log(self, text: str) -> None:
        print(text, file=sys.stderr, flush=True)

    def send_body(self, status: int, body: bytes, ctype: str = "application/json",
                  close: bool = False) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, status: int, message: str) -> None:
        self.send_body(status, json.dumps(
            {"error": {"message": message, "type": "llm_gate", "code": status}}).encode())

    def authorized(self) -> bool:
        keys = self.server.keys
        if not keys or self.path.split("?")[0].rstrip("/").endswith("/health"):
            return True
        auth = self.headers.get("Authorization", "")
        token = auth[7:].strip() if auth.lower().startswith("bearer ") else ""
        return any(hmac.compare_digest(token.encode(), k.encode()) for k in keys)

    def do_GET(self) -> None:
        self.handle_request(None)

    def do_POST(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > self.server.max_body:
            self.close_connection = True
            self.send_error_json(413, "bad or too large request body")
            return
        self.handle_request(self.rfile.read(length))

    def handle_request(self, body: bytes | None) -> None:
        """Answer one request; body is None for a GET."""
        if not self.authorized():
            self.send_error_json(401, "invalid API key")
            return
        path = self.path.split("?")[0].rstrip("/")
        if body is not None and path.endswith("/chat/completions"):
            try:
                request: dict[str, Any] = json.loads(body)
                messages: list[Message] = request["messages"]
                if not isinstance(messages, list):
                    raise TypeError
            except (ValueError, KeyError, TypeError):
                self.send_error_json(400, "request is not a chat completion")
                return
            gate = self.server.gate
            messages, ask = gate.apply(messages)
            said = text_of(messages[-1]) if messages else ""
            if not ask:
                self.log("refused" + (": " + said if self.server.verbose else ""))
                stream = bool(request.get("stream"))
                self.send_body(
                    200, canned_completion(request.get("model", ""), gate.reply, stream),
                    "text/event-stream" if stream else "application/json")
                return
            self.log("passed" + (": " + said if self.server.verbose else ""))
            if not self.server.keep_time:
                messages = without_time(messages)
            request["messages"] = messages
            body = json.dumps(request).encode()
        self.forward(body)

    def forward(self, body: bytes | None) -> None:
        """Send the request on to llama-server and relay its answer."""
        backend = self.server.backend
        conn = http.client.HTTPConnection(self.server.backend_host, backend.port or 80,
                                          timeout=self.server.timeout_s)
        headers: dict[str, str] = {"Accept": self.headers.get("Accept", "*/*")}
        if body is not None:
            headers["Content-Type"] = "application/json"
        try:
            try:
                conn.request(self.command, backend.path.rstrip("/") + self.path,
                             body=body, headers=headers)
                resp = conn.getresponse()
            except OSError as err:
                self.log("backend: %s" % err)
                self.send_error_json(502, "llama-server is not reachable: %s" % err)
                return
            ctype = resp.getheader("Content-Type", "application/json")
            if not ctype.startswith("text/event-stream"):
                self.send_body(resp.status, resp.read(), ctype)
                return
            # A stream: hand on every piece as it arrives. If Home Assistant
            # goes away, closing the backend connection stops the generation.
            self.send_response(resp.status)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.close_connection = True
            self.end_headers()
            try:
                while True:
                    piece = resp.read1(65536)
                    if not piece:
                        break
                    self.wfile.write(piece)
                    self.wfile.flush()
            except OSError:
                pass
        finally:
            conn.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--listen", default="0.0.0.0:8090", metavar="HOST:PORT")
    ap.add_argument("--backend", default="http://127.0.0.1:8080", metavar="URL",
                    help="llama-server, without /v1 (default %(default)s)")
    ap.add_argument("--keep", default="", metavar="WORDS",
                    help="trigger words that stay in the text, comma-separated")
    ap.add_argument("--strip", default="", metavar="WORDS",
                    help="trigger words that are removed from the text")
    ap.add_argument("--reply", default="Sorry, I couldn't understand that",
                    help="answer to a sentence without a trigger word")
    ap.add_argument("--keep-time", action="store_true",
                    help="leave the time of day in the system prompt")
    ap.add_argument("--api-key-file", metavar="FILE",
                    help="accept only requests with one of these keys (one per line)")
    ap.add_argument("--timeout", type=float, default=300, metavar="SECONDS",
                    help="give up on llama-server after this long without data")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log the text of each sentence, not only the decision")
    args = ap.parse_args()

    def words(s: str) -> list[str]:
        return [w.strip() for w in s.split(",") if w.strip()]

    backend = urllib.parse.urlsplit(args.backend)
    if not backend.hostname:
        ap.error("--backend must be a URL such as http://127.0.0.1:8080")
    host, _, port = args.listen.rpartition(":")
    server = GateServer((host, int(port)), Handler)
    server.gate = Gate(words(args.keep), words(args.strip), args.reply)
    server.backend = backend
    server.backend_host = backend.hostname
    server.timeout_s = args.timeout
    server.verbose = args.verbose
    server.keep_time = args.keep_time
    server.max_body = 8 << 20
    server.keys = []
    if args.api_key_file:
        with open(args.api_key_file) as f:
            server.keys = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    print("llm-gate: %s -> %s, keep %s, strip %s%s" % (
        args.listen, args.backend, sorted(server.gate.keep), sorted(server.gate.strip),
        ", API key required" if server.keys else ""), file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
