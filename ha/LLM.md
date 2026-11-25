# Questions to a language model

A sentence that starts with **explain**, **question** or **LLM** is answered
by a language model running under llama.cpp on a machine of your own, and the
answer is spoken while the model is still writing it. Everything else stays
with Home Assistant's own sentence matching, as before.

**Status:** tried on a PC with Home Assistant 2026.11.x
llama.cpp build 12648, `unsloth/Qwen3.5-4B-GGUF:UD-Q4_K_XL` and Piper 2.5.4,
all in Docker, with sentences typed into the voice pipeline.

## How it fits together

```
Echo ── Home Assistant voice pipeline
          1. speech-to-text
          2. sentence triggers, then Home Assistant's own sentences
          3. anything left ──> llama.cpp integration ──> llm-gate ──> llama-server
                                                         port 8090     port 8080, local only
          4. Piper, fed with the answer as it arrives
```

- **llama-server** runs the model.
- **llm-gate.py** (this folder) sits in front of it. Home Assistant sends the
  conversation agent every sentence its own matcher did not understand, so a
  misheard "turn on the kitchen pan" would be answered by the model. The gate
  lets a sentence through only if it starts with a trigger word and answers
  "Sorry, I couldn't understand that" itself for the rest, without the model.
- Home Assistant has no setting for this. An automation with a sentence
  trigger (`explain {query}`) that calls `conversation.process` and returns
  the result with `set_conversation_response` does route by word, but its
  answer reaches the speech engine in one piece after the model has
  finished. Tried with the same model and machine: first audio after 8 to
  14 s, against 2.5 to 3.5 s with the gate.

## On the machine with llama.cpp

As root, with the files from this folder:

```sh
useradd --system --home /var/lib/llama --shell /usr/sbin/nologin llama
mkdir -p /var/lib/llama /opt/llm-gate && chown llama: /var/lib/llama
cp llm-gate.py /opt/llm-gate/
cp llama-server.service llm-gate.service /etc/systemd/system/
# In llama-server.service set the path of your llama-server (which llama-server).
systemctl daemon-reload
systemctl enable --now llama-server llm-gate
journalctl -u llama-server -f     # first start downloads 2.9 GB; wait for "listening"
```

The model is downloaded again into `/var/lib/llama`. To use the file you
already have, replace `-hf ... --no-mmproj` in the service file with
`-m /path/to/Qwen3.5-4B-UD-Q4_K_XL.gguf` and make it readable for the user
`llama`.

Check, on that machine:

```sh
curl -s http://127.0.0.1:8090/v1/models
curl -s http://127.0.0.1:8090/v1/chat/completions \
    -d '{"messages":[{"role":"user","content":"Question, how far is the moon?"}]}'
curl -s http://127.0.0.1:8090/v1/chat/completions \
    -d '{"messages":[{"role":"user","content":"turn on the kitchen pan"}]}'
```

The second prints an answer from the model with no `reasoning_content` in it,
the third "Sorry, I couldn't understand that".

Thinking has to be off. With this model file and llama.cpp build 12648 it is
on unless switched off (Unsloth's guide says the opposite for the 4B model),
and the answer then starts only after the thinking text. `--reasoning off` in
the service file does it; an older llama-server without that option takes
`--chat-template-kwargs '{"enable_thinking":false}'`. Both were tried.

## In Home Assistant

Needs 2026.8 or newer, which has the **llama.cpp** integration built in.

1. Settings → Devices & services → Add integration → **llama.cpp**.
   - URL: `http://<address of the machine>:8090/v1`
   - API key: empty, unless you set one in `llm-gate.service`.
   - Model: `qwen3.5-4b`.

   Both services must be running: Home Assistant tests at this moment whether
   the server can stream and remembers the result.
2. On the integration's page open the conversation agent "Qwen3.5 4b"
   (Reconfigure) and set:
   - **Instructions:** the text below.
   - **AI tools:** untick "Device control". It is ticked by default and adds
     the tool descriptions and your exposed devices to every request. Devices
     are handled by Home Assistant's own sentences anyway. How much slower
     the model gets with it was not measured.
   - **Recommended model settings:** off, then Maximum tokens 200,
     Top P 0.8, Temperature 0.7 (Qwen's values for answers without thinking).
3. Settings → Voice assistants → your assistant:
   - Conversation agent: "Qwen3.5 4b".
   - "Prefer handling commands locally": on.
   - Text-to-speech: Piper. The answer is only streamed with a speech engine
     that accepts text piece by piece.

Instructions:

```
You are the voice of a smart speaker. Your answer is read aloud by a text-to-speech engine, and the question reached you through speech recognition.
- Begin with the answer itself, in a short first sentence. No greeting, no repeating the question, no closing remark.
- Use one to three short sentences. Only when asked to explain something, use up to six.
- Plain spoken English only: no lists, headings, markdown, emoji, brackets or abbreviations. Write numbers and units as they are spoken.
- The question may contain recognition errors. Answer its most likely meaning.
- If you do not know or are not sure, say so in one sentence. Never invent facts.
- Do not ask questions back.
- You cannot control devices or look anything up.
```

The first sentence matters for speed: Piper speaks sentence by sentence, so
the first audio comes when the first sentence is complete.

### Check that it streams

Settings → Voice assistants → your assistant → three dots → Debug, type
"Explain why the sky is blue" and look at the raw data of the run:

- `tts_output.stream_response: true` in `run-start`. If it is `false`, the
  speech engine does not take streamed text, or the integration was added
  while the server could not stream (remove and add it again).
- an `intent-progress` event with `tts_start_streaming: true`. Home Assistant
  sends it once the answer is longer than 60 characters; shorter answers are
  spoken in one piece.

## Trigger words

| Said | Reaches the model as |
|---|---|
| "Explain why the sky is blue." | Explain why the sky is blue. |
| "Question, who wrote War and Peace?" | Who wrote War and Peace? |
| "LLM, how far is the moon?" | How far is the moon? |

- The words are set in `llm-gate.service`: `--keep` for words that stay in
  the sentence, `--strip` for words that are removed. No words at all opens
  the gate for everything.
- Punctuation and spelling of the recogniser do not matter. Qwen3-ASR wrote
  the three words, spoken by three Piper voices, as "Explain", "Question:"
  and, for the third, "LLM", "L L M" or "L. L. M."; all of these match.
- If the model's answer ends with a question mark, Home Assistant listens
  again without the wake word, and the reply to it passes without a trigger
  word. The instructions tell the model not to ask.
- A word that Home Assistant's own sentences also understand never arrives
  here: what Home Assistant matches, it answers.
- `journalctl -u llm-gate` shows "passed" or "refused" per sentence; with
  `-v` in the service file also the text, which is how to see what the
  recogniser made of a word.

## What to expect

Measured on a Ryzen 7 PRO 5850U (8 threads) with llama-server, Piper and Home
Assistant on the same machine, sentence typed into the pipeline, so without
the microphone and speech-to-text. Times from the moment Home Assistant has
the sentence.

| | |
|---|---|
| First text from the model | 0.65 to 0.75 s |
| The same for the first question after a start of llama-server, and after midnight | 3.5 to 4 s |
| The same for a second question in the same conversation | 2.3 s |
| Home Assistant starts feeding Piper (60 characters) | 2.0 to 2.6 s |
| First audio available | 2.5 to 3.5 s |
| Answer of three to five sentences complete | 8 to 13 s |
| Sentence without a trigger word refused | 0.01 s |

- The model wrote 6 to 8.5 tokens per second and read 50 to 60. Without
  streaming the first audio would come when the answer is complete.
- A conversation lasts 5 minutes on the satellite (`conversation_timeout_s`).
  Within it the model sees the earlier questions and answers, which makes
  "and how efficient is it?" work, and it reads the previous question and
  answer again each time; that is the 2.3 s.
- The i7-9700T was not measured.

### Why the gate removes the time

Home Assistant appends "Current time is 12:34:56. Today's date is ..." to the
instructions, so the prompt changes with every request. Qwen3.5 cannot reuse
part of a prompt it has read, only return to a checkpoint that llama-server
made at the end of the instructions, and the changed time lies before that
point. The whole prompt was then read again every time: 3.5 to 4 s to the
first text instead of 0.7 s. The gate deletes the time and leaves the date
(`--keep-time` turns that off). "What time is it" is answered by Home
Assistant itself.

## Other models, and the llama.cpp version

Compared on 2026-11 on the same PC, CPU only, 8 threads, llama.cpp
build 12680, directly against llama-server with the instructions above: 16
questions per run (facts, explanations, arithmetic, unit conversion, a town
that does not exist, a misrecognised sentence, a date calculation), judged by
hand. Times are medians and count from the request.

| Model (4-bit file) | Writes | First text | First sentence complete | Whole answer | Right of 16 |
|---|---|---|---|---|---|
| Qwen3.5-4B `UD-Q4_K_XL`, 2.9 GB | 8 tokens/s | 0.8 s | 2.5 s | 6.7 s | 14, 15, 12 |
| Gemma 4 E2B `Q4_K_M`, 3.1 GB | 17 tokens/s | 0.16 s | 1.0 s | 1.7 s | 14, 13, 13, 14 |
| Gemma 4 E4B `Q4_K_M`, 5.0 GB | 9 tokens/s | 0.5 s | 1.8 s | 3.3 s | 12 |
| Qwen3.5-2B `UD-Q4_K_XL`, 1.3 GB | 20 tokens/s | 0.4 s | 1.1 s | 1.4 s | about 8 |
| LFM3-1.7B `Q4_K_M`, 1.1 GB | 25 tokens/s | | | | not usable, see below |

- **Gemma 4 E2B** was as often right as Qwen3.5-4B and twice as fast, with
  answers half as long (130 against 250 characters), which is most of the
  difference in the last time column. A second question in the same
  conversation started after 0.35 s against 2.5 s. Its own mistakes: it
  garbled the distance to the moon in two of four runs when writing the
  number in words, and it never understood "the hide of mount ever rest" as
  "height" (Qwen3.5-4B did in two of three runs).
- Qwen3.5-4B's mistakes were numbers: 350 °F in Celsius wrong in two of three
  runs, the boiling point on Everest wrong once.
- Nobody could count the days until Christmas.
- **LFM3-1.7B** put its thinking into the answer text in 5 of 16 answers with
  `--reasoning off` and once answered with a tool call; with reasoning left
  on it thought for 8 s per question. Whether it has a switch for that was
  not looked into.
- 16 questions and a few runs are a small sample. There is no small Qwen
  newer than 3.5: Qwen3.6 and 3.8 exist only from 27 B upwards.
- Not run through Home Assistant and Piper; from the parts above the first
  audio with Gemma 4 E2B should come after about 1.5 s instead of 2.5 to
  3.5 s.

To use Gemma 4 E2B, change in `llama-server.service`:

```
    -hf unsloth/gemma-4-E2B-it-GGUF:Q4_K_M --no-mmproj \
    --host 127.0.0.1 --port 8080 --alias gemma-4-e2b \
    --ctx-size 8192 --parallel 1 --threads 8 \
    --reasoning off --top-k 64
```

and in Home Assistant select the model `gemma-4-e2b` in the conversation
agent, with Temperature 1.0 and Top P 0.95 (Google's values). The gate and
the instructions stay as they are.

**llama.cpp**: the default build is the right one for a CPU: `cmake -B build && cmake
--build build -j`. To check an existing build,
`grep -E 'CMAKE_BUILD_TYPE|GGML_NATIVE|GGML_OPENMP:' build/CMakeCache.txt`
should show `Release`, `ON`, `ON`, and `llama-server -lv 4 ...` prints a
`system_info` line at the start that should contain `AVX2 = 1`, `FMA = 1`
and `OPENMP = 1`. Link-time optimisation and BLAS were not tried.

## Not tested

- **The API key file** with Home Assistant; the gate's check itself was.

## Limits

- The gate applies to everything that uses this conversation agent, also the
  Assist chat in the browser.
- A 4B model can be wrong now and then.
- It cannot control devices and does not know anything current.
- Without the gate (Home Assistant pointed at llama-server directly, started
  with `--host 0.0.0.0`) every sentence that is not understood goes to the
  model, and each answer starts about 3 s later because of the time in the
  prompt.
