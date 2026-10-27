# Agent directory model

A bounded model for device pickers over the Gateway's `agents.list` response.
It retains eight rows at a time: canonical ID, display name and identity emoji.
The identity name takes precedence over the configured name; a missing name
falls back to the ID. Remote avatar URLs and unrelated metadata are not retained.

`esp_openclaw_directory_parse` accepts at most 32 KiB and 16 levels of JSON
nesting. Names and emoji have UTF-8-safe byte limits. IDs must already be
canonical lowercase identifiers of at most 64 bytes; they are never shortened.
Invalid UTF-8, embedded NULs, invalid IDs, duplicate IDs, malformed responses
and out-of-range page offsets
reject the page. An empty successful roster is distinct from a parse failure.

The current [Gateway contract](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/packages/gateway-protocol/src/schema/agents-models-skills.ts#L145)
accepts an empty parameter object and returns the whole roster. The offset is
local paging, not server-side pagination. Every refresh still receives and
parses the whole response, subject to the device's transport and memory limits.
Listing an agent does not establish its availability for a later operation;
the Gateway must authorize that operation separately.

This component owns no network client, selection, credentials or persistence.
The caller must reject responses from replaced connections before exposing a
page. The Dial's host runner exercises the parser and that lifecycle together:

```sh
python3 examples/m5stack-dial-node/tests/run_reader_host_tests.py \
  --cjson-dir examples/m5stack-dial-node/managed_components/espressif__cjson/cJSON
```
