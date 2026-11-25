# Debugging Home Assistant Assist with text

How to see what Assist understands and which entities it sees, without
speaking to the Echo. Home Assistant has three built-in places for this, plus
the list of exposed entities.

The menu names are from memory and were not checked against a running Home
Assistant, so a label may differ slightly in your version.

## 1. What a sentence is understood as: the sentence parser

Developer tools → **Assist** tab. Type a sentence and it shows, without
executing anything:

- which intent matched (for example `HassTurnOff`), or that nothing matched;
- the slots it extracted (name, area, domain);
- which entities it would act on.

This is the best tool for "why did it pick that fan" or "why does it not know
this phrase".

## 2. What a whole request did: pipeline debug

Settings → Voice assistants → your assistant → three-dot menu → **Debug**.

- It lists recent runs, including those from the Echo, stage by stage: the
  text speech-to-text produced, the intent result, the response text, and the
  raw event data.
- You can also run the pipeline from typed text there, which skips the
  microphone and speech-to-text.
- Use it to tell a recognition error ("pan" for "fan") from an intent error.

For quick tries, the Assist chat icon at the top right of a dashboard takes
typed text and really executes the command.

## 3. The raw answer: the `conversation.process` action

Developer tools → Actions:

```yaml
action: conversation.process
data:
  text: turn off the kitchen fan
  language: en
```

The response shows `response_type` (`action_done`, `query_answer` or
`error`), an error code such as `no_intent_match` or `no_valid_targets`, and
the lists of entities that succeeded and failed.

## 4. Which entities it sees

Settings → Voice assistants → **Expose** tab. Assist only knows the entities
listed there.

- Entities are matched by their name, their aliases and their area. Aliases
  are set per entity in the same place, or in the entity's settings under
  "Voice assistants".
- An entity that is not exposed, or whose spoken name is neither its name nor
  an alias, gives `no_valid_targets`.
- Areas and floors can have aliases too, in Settings → Areas.

## 5. Which sentences exist

The built-in sentences are in the `home-assistant/intents` repository on
GitHub, under `sentences/en/`. Your own go in
`config/custom_sentences/en/<anything>.yaml`, and the parser in point 1 picks
them up after a reload of the conversation integration or a restart.

## More detail in the log

Add this to `configuration.yaml` if the tools above are not enough:

```yaml
logger:
  logs:
    homeassistant.components.conversation: debug
    homeassistant.components.assist_pipeline: debug
    homeassistant.components.intent: debug
```
