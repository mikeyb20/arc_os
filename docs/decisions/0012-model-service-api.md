# ADR-0012: Models only through `arc-llm`

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
- One service, `arc-llm`, owns model loading (llama.cpp, GGUF, CPU only). Apps never open model files.
- Transport: AF_UNIX socket `/var/run/arc-llm.sock`; HTTP/1.1 with OpenAI-style JSON
  (`/v1/chat/completions`, `/v1/embeddings`, `/v1/models`) plus `/v1/tokenize`. Streaming via chunked responses.
- Roles (`chat`, `embed`, `router`, `expert:<domain>`) map to installed models in `/models/roles.json`, chosen by the user.
- Install refuses or warns when a model's `ram_mb` exceeds what the machine can give it.
- Upgrade = install beside, switch role, keep the old model until removed.
- Assistant tool calls use a versioned tool list (`search_packs`, `open_source`, `read_file`, `list_dir`,
  `run_command` with user confirmation).

## Consequences
- Users swap or upgrade models with no app changes; a different inference engine can replace llama.cpp later.
- Rules out: linking llama.cpp into apps directly.

## Verification
- M2 done-test (`ask` command); a test that switches the `chat` role between two models.
