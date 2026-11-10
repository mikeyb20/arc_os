# M2-04 — `arc-llm` service

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| L | D | L | M2-03 | M2-04b, M2-05, M3-02, M5-02 |

**Goal.** The one program that loads models (ADR-0012): `arc-llm` serves `generate`, `embed`, `tokenize` and
`list_models` as OpenAI-style JSON over HTTP/1.1 on an AF_UNIX socket, picks models by role from
`/models/roles.json`, refuses models that don't fit in RAM, and runs on Linux inside the syscall inventory.

**Read first.**
- `docs/plans/lane-l.md`, `docs/decisions/0012-model-service-api.md`, `docs/plans/knowledge-os.md` §7
- `schemas/model.schema.json`, `schemas/model-roles.schema.json`, `schemas/examples/model.valid-*.json`
- `llama.h` from the M2-03 port, Notes of M2-03

**Build.**
1. `stack/lib/arcjson/`: a JSON parser (RFC 8259, UTF-8 checked, nesting limit 64, input size limit given by the
   caller, parse into one arena so freeing is one call) and a writer (escaping, numbers, nested objects and arrays
   into a growing buffer). Host-style lane test `lane.arcjson` with RFC edge cases: escapes, surrogate pairs, bad
   UTF-8, deep nesting, numbers at the limits, trailing garbage. This is the stack's only JSON code; M3-02 and
   M5-02 use it. Add it to the table in `stack/README.md`.
2. `stack/arc-llm/`, split by job: `main.c` (arguments `--root`, `--socket`, `--threads`, `--ctx`, `--reserve-mb`;
   signals), `http.c` (request line, headers, `Content-Length` bodies up to 1 MiB; responses with
   `Content-Length`, or chunked for streams; one request per connection, `Connection: close`), `roles.c`
   (`/models/roles.json` and `/models/<name>/<version>/model.json` through arcjson), `engine.c` (llama.h:
   load, contexts, sampling, embeddings, tokenize), `api.c` (the four endpoints).
3. Endpoints (errors are `{"error":{"message","type"}}` with a fitting HTTP status):
   - `POST /v1/chat/completions`: `model` (a role such as `chat` or `expert:medicine`, or a model name; default
     `chat`), `messages`, `max_tokens`, `temperature` (0 = greedy), `seed`, `stream`. The prompt is built with
     `llama_chat_apply_template` from the model's `chat_template` in `model.json` (GGUF's own template if absent).
     `stream: true` sends server-sent events (`data: {…}` chunks, then `data: [DONE]`).
   - `POST /v1/embeddings`: `input` (string or array of strings), `model` (default `embed`); L2-normalised floats.
   - `POST /v1/tokenize`: `content`, `model` (default `chat`) → `{"tokens":[…]}`.
   - `GET /v1/models`: every model named in `roles.json`, with its roles, `ram_mb` and whether it is loaded.
4. Behaviour:
   - One request at a time; other clients wait in the listen backlog. No HTTP threads; llama.cpp's own thread pool
     does the compute (`--threads`, default: CPUs from `sched_getaffinity`).
   - Models load on first use (mmap, read-only) and stay loaded. Each loaded model keeps one context, cleared
     between requests. `n_ctx` = min(`context_length` from `model.json`, `--ctx`, default 4096).
   - Fit check before loading: `ram_mb` of this model + models already loaded + `--reserve-mb` (default 1024, for
     the OS and page cache; M2-06 measures the real figure) must not exceed RAM from `sysinfo`. Otherwise HTTP 507
     with the numbers in the message; never load and hope.
   - Before each request, `stat` `roles.json`; if its mtime changed, re-read it and unload models no role names any
     more. Role switches (M5-02's `arcpkg role`) need no restart and no signal.
   - Writes use `MSG_NOSIGNAL`; a client that disconnects mid-stream stops generation at the next token.
   - `SIGTERM`/`SIGINT`: finish the current token, close and unlink the socket, exit 0.
5. Lane tests in `stack/tests/arc-llm/`: a driver (shell or Python, untraced) builds a temporary root holding the
   two archived tiny models as installed packages (`model.json` per model, `roles.json`), starts `arc-llm` through
   `tools/lane/run.sh`, and talks to it with `curl --unix-socket`:
   - greedy chat twice → identical text; streaming gives the same text as non-streaming;
   - embeddings → cosine ≥ 0.9999 against `llama-embedding` (M2-03) for the same text; tokenize → the same IDs as
     the M2-03 tokenize tool;
   - role switch: a second version of the chat model (same GGUF, new version directory) is named in `roles.json`;
     the next request uses it and `/v1/models` shows it (ADR-0012's verification test);
   - fit check: `--reserve-mb` larger than RAM → 507, process still serving;
   - errors: bad JSON → 400, unknown role → 404, body over 1 MiB → 413, unknown path → 404;
   - two clients at once both get correct answers, one after the other; a client that drops mid-stream doesn't
     stop the server; `SIGTERM` removes the socket file.
6. Expected inventory additions (add the rows, owners in brackets): `socket` with `family: AF_UNIX`, `bind`,
   `listen`, `accept`/`accept4`, `sendto`/`recvfrom` as musl emits them (M1-18), `rt_sigaction` for `SIGPIPE` if
   used (M1-01). Use what the log shows, not this list.
7. Budgets: `arc_llm_bytes`; `arc_llm_idle_rss_kib` (no model loaded); `arc_llm_tiny_rss_kib` (after one chat and
   one embed request on the tiny models).

**Done when.**
- [x] Every lane test passes and the syscall check passes in `lane.yml`.
- [x] `stack/README.md` lists arcjson; the inventory additions name their M1 owner blocks.

**Cost.** Per request: one JSON parse O(body), one arena; prompt and output token buffers sized from `n_ctx`. Per
loaded model: the mmapped weights (page cache, shared) plus one KV cache (`n_ctx` × layers × KV width, logged at load
time). No per-request heap growth that survives the request; idle RSS has a budget. No polling or timers while idle:
the process sleeps in `accept`.

**Watch out.**
- The tiny chat model's template is ChatML; `llama_chat_apply_template` only knows built-in templates. A model
  whose template it can't match must fail the request with a clear error, not produce garbage.
- `nomic-embed-text` expects `search_query: ` / `search_document: ` prefixes. That is the caller's job (M3-06);
  `arc-llm` passes text as given.
- The socket path must fit `sun_path` (108 bytes); temporary roots in tests must be short.
- Big, but don't split the JSON library out: M3-02 and M5-02 need it, and nothing else would build it earlier.
  If the block passes 1.5× its size, hand off with streaming and the role-switch test as the second session.

**Notes.**
- Done in one session (2026-11-10), no hand-off. `stack/lib/arcjson/` (+ `lane.arcjson`), `stack/arc-llm/` split as
  specified, six driver tests in `stack/tests/arc-llm/arc_llm_lane.py` (chat, embed, roles, fit, errors, clients).
  Embeddings match `llama-embedding` at cosine 1.0000000; tokenize matches `llama-tokenize`; stream == non-stream.
- Inventory: every addition is M1-18 (socket AF_UNIX, connect, recvfrom, sendmsg, shutdown, bind, listen,
  setsockopt, accept4); `syscall-table.md` rows added. No SIGPIPE handler: `MSG_NOSIGNAL` covers it.
- Choices the spec left open: responses carry `model_version` (the role-switch test needs it); `temperature`
  defaults to 0.8 with llama.cpp's default top-k 40 / top-p 0.95 / min-p 0.05 chain, 0 is greedy; `max_tokens`
  defaults to the rest of the context; a role whose model.json doesn't list it, or a bad model.json, gives 500
  with the reason; wrong method gives 405.
- Added for robustness (flag if unwanted): 30 s `SO_RCVTIMEO`/`SO_SNDTIMEO` per client, so a stuck client can't
  block the one-at-a-time service (a timer only while a client is connected, none while idle); a stale socket file
  is replaced, a live one makes a second `arc-llm` exit 1.
- SIGTERM race: the handler also `shutdown()`s the listening socket, so a signal landing between the flag check
  and `accept4` still ends the loop. M1-18 must give `accept` on a shut-down listener an error (Linux: `EINVAL`).
- `roles.json` change detection uses inode, size and mtime; ext2 mtimes are whole seconds, so writers must
  replace it by rename (now said in lane-l.md, for M5-02).
- Prompt text is tokenized with special tokens parsed (as llama-server does): a user message containing
  `<|im_end|>` can end its turn early. Fine for one local user; revisit if untrusted input ever reaches `arc-llm`.
- Measured: `arc-llm` 7.67 MB loaded; idle RSS 16 MB; both tiny models loaded 408 MB (KV 90 MiB, compute
  buffers 107 + 110 MiB; the embed context uses n_ubatch = n_ctx). Next lane block: M3-02, after the ADR-0020
  maintenance check Michael asked for.
