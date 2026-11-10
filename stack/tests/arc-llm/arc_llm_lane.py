#!/usr/bin/env python3
"""Lane tests for arc-llm (M2-04): the model service on the archived tiny models.

    arc_llm_lane.py <test> --arc-llm PROG --tools DIR --chat GGUF --embed GGUF

DIR holds the M2-03 llama.cpp tools (llama-embedding, llama-tokenize). Each test builds a short
temporary root (/tmp/arcllm*/: the socket path must fit sun_path, 108 bytes) holding both models
as installed packages (models/<name>/<version>/model.json, models/roles.json), starts arc-llm in
it through $ARC_LANE_RUN (traced for the syscall check, log arc_llm_<test>), and talks to it with
curl --unix-socket. <test> is one of:

    chat     greedy chat twice: identical text; streaming gives the same text as non-streaming;
             then one embed request and SIGTERM: socket removed, exit 0. Writes the
             arc_llm_tiny_rss_kib budget (peak RSS with both models loaded) to arc_llm_chat.budget.log.
    embed    embeddings: cosine >= 0.9999 against llama-embedding; tokenize: the same IDs as
             llama-tokenize.
    roles    role switch: chat moves to a second version of the chat model (same GGUF, new
             version directory); the next request uses it and /v1/models shows it (ADR-0012).
    fit      --reserve-mb larger than RAM: chat gets 507 with the numbers; still serving. Also:
             starts over a stale socket file; a second arc-llm on a live socket refuses to start.
    errors   bad JSON 400, unknown role 404, body over 1 MiB 413, unknown path 404, wrong
             method 405. Writes arc_llm_idle_rss_kib (peak RSS, no model ever loaded) to arc_llm_errors.budget.log.
    clients  two clients at once both get the right answer; a client that drops mid-stream
             stops generation and the server keeps serving.

Drivers run untraced; only arc-llm and the llama.cpp tools go through $ARC_LANE_RUN.
"""

import argparse
import atexit
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "llama"))
import llama_lane  # noqa: E402  (run, cosine: one home per helper)

CHAT = {"messages": [{"role": "system", "content": "You are a helpful assistant."},
                     {"role": "user", "content": "How do I make water safe to drink?"}],
        "max_tokens": 32, "temperature": 0}
EMBED_TEXTS = llama_lane.EMBED_TEXTS
TOKENIZE_TEXT = llama_lane.TOKENIZE_TEXT
START_TIMEOUT = 60  # seconds; arc-llm starts in well under one, strace and TCG slow it down
CHAT_TEMPLATE = ("{% for message in messages %}<|im_start|>{{ message.role }}\n{{ message.content }}<|im_end|>\n"
                 "{% endfor %}<|im_start|>assistant\n")

failures = 0


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


def install_model(root, name, version, gguf, manifest):
    d = os.path.join(root, "models", name, version)
    os.makedirs(d)
    os.symlink(gguf, os.path.join(d, "model.gguf"))
    doc = {"schema_version": 1, "name": name, "version": version, "file": "model.gguf", "sha256": "0" * 64,
           "license": "Apache-2.0", "source": "third_party/SOURCES.lock (lane test)", **manifest}
    with open(os.path.join(d, "model.json"), "w") as f:
        json.dump(doc, f)


def write_roles(root, roles):
    """Write roles.json the way an installer must: a new file renamed over the old one."""
    path = os.path.join(root, "models", "roles.json")
    with open(path + ".new", "w") as f:
        json.dump({"schema_version": 1, "roles": roles}, f)
    os.rename(path + ".new", path)


def make_root(args):
    root = tempfile.mkdtemp(prefix="arcllm", dir="/tmp")
    os.makedirs(os.path.join(root, "var", "run"))
    chat = {"roles": ["chat"], "parameter_count": 134515008, "quantization": "Q8_0", "ram_mb": 400,
            "context_length": 8192, "chat_template": CHAT_TEMPLATE}
    install_model(root, "smollm2-135m-instruct", "1.0.0", args.chat, chat)
    install_model(root, "smollm2-135m-instruct", "1.1.0", args.chat, chat)
    install_model(root, "nomic-embed-text", "1.5.0", args.embed,
                  {"roles": ["embed"], "parameter_count": 136731648, "quantization": "Q8_0", "ram_mb": 300,
                   "context_length": 2048, "embedder": {"id": "nomic-embed-text-v1-5", "dimensions": 768}})
    write_roles(root, {"chat": {"name": "smollm2-135m-instruct", "version": "1.0.0"},
                       "embed": {"name": "nomic-embed-text", "version": "1.5.0"}})
    return root


class Server:
    """arc-llm in a temporary root, started through $ARC_LANE_RUN."""

    def __init__(self, args, test, extra=(), stale_socket=False):
        self.root = make_root(args)
        self.sock = os.path.join(self.root, "var", "run", "arc-llm.sock")
        if stale_socket:  # what a crashed arc-llm leaves behind: a socket file nobody listens on
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as dead:
                dead.bind(self.sock)
        self.log = tempfile.TemporaryFile()
        cmd = [os.environ["ARC_LANE_RUN"], f"arc_llm_{test}", args.arc_llm, "--root", self.root,
               "--threads", "4", *extra]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        atexit.register(self.cleanup)
        deadline = time.monotonic() + START_TIMEOUT
        while not self.listening():
            if self.proc.poll() is not None or time.monotonic() > deadline:
                self.dump_log()
                raise SystemExit("FAIL: arc-llm did not start")
            time.sleep(0.05)

    def listening(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            return probe.connect_ex(self.sock) == 0

    def pid(self):
        """arc-llm's own pid: under strace it is strace's child."""
        try:
            with open(f"/proc/{self.proc.pid}/task/{self.proc.pid}/children") as f:
                children = f.read().split()
        except OSError:
            children = []
        return int(children[0]) if children else self.proc.pid

    def stop(self):
        """SIGTERM arc-llm; returns (exit status, peak RSS in KiB)."""
        os.kill(self.pid(), signal.SIGTERM)
        _, status, usage = os.wait4(self.proc.pid, 0)
        self.proc.returncode = os.waitstatus_to_exitcode(status)
        return self.proc.returncode, usage.ru_maxrss

    def cleanup(self):
        """Kill arc-llm if a test failed before stopping it; remove the temporary root."""
        if self.proc.returncode is None:
            self.proc.kill()
            self.proc.wait()
        shutil.rmtree(self.root, ignore_errors=True)

    def stderr(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", errors="replace")

    def dump_log(self):
        sys.stdout.write("--- arc-llm stderr ---\n" + self.stderr()[-4000:] + "\n---\n")

    def request(self, path, body=None, raw=None, method=None):
        """One request with curl; returns (HTTP status, response text)."""
        cmd = ["curl", "-sS", "--unix-socket", self.sock, "-o", "-", "-w", "\n%{http_code}", "--max-time", "120"]
        if method:
            cmd += ["-X", method]
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        if data is not None:
            cmd += ["-H", "Content-Type: application/json", "--data-binary", "@-"]
        cmd.append("http://localhost" + path)
        out = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, check=False).stdout.decode("utf-8", "replace")
        text, _, code = out.rpartition("\n")
        return int(code) if code.isdigit() else 0, text

    def json(self, path, body=None, **kw):
        status, text = self.request(path, body, **kw)
        try:
            return status, json.loads(text)
        except ValueError:
            return status, {"raw": text}

    def chat_text(self, body):
        status, doc = self.json("/v1/chat/completions", body)
        if status != 200:
            print(f"  chat: {status} {doc}")
            return None, doc
        return doc["choices"][0]["message"]["content"], doc


def stream_text(server, body):
    """Streamed chat: the concatenated deltas, the finish reason, and whether [DONE] came."""
    status, text = server.request("/v1/chat/completions", dict(body, stream=True))
    pieces, finish, done = [], None, False
    for line in text.split("\n"):
        if not line.startswith("data: "):
            continue
        if line == "data: [DONE]":
            done = True
            continue
        choice = json.loads(line[6:])["choices"][0]
        pieces.append(choice["delta"].get("content", ""))
        finish = choice["finish_reason"] or finish
    return status, "".join(pieces), finish, done


def write_budget(suite, name, value):
    budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
    if budget_dir:
        with open(os.path.join(budget_dir, f"{suite}.budget.log"), "w") as f:
            f.write(f"ARC_TEST budget {name} {value}\n")
    print(f"{name} = {value}")


def test_chat(args):
    s = Server(args, "chat")
    first, doc = s.chat_text(CHAT)
    second, _ = s.chat_text(CHAT)
    print(f"reply: {first!r}")
    check(first is not None and len(first) > 0, "greedy chat answers")
    check(first == second, "greedy chat twice: identical text")
    check(doc.get("usage", {}).get("completion_tokens", 0) > 0 and doc["choices"][0]["finish_reason"] in
          ("stop", "length"), "usage and finish_reason filled in")
    status, streamed, finish, done = stream_text(s, CHAT)
    check(status == 200 and done, "stream: 200, ends with [DONE]")
    check(streamed == first and finish == doc["choices"][0]["finish_reason"],
          "stream: same text and finish_reason as non-streaming")
    status, emb = s.json("/v1/embeddings", {"input": EMBED_TEXTS[0]})
    check(status == 200 and len(emb["data"][0]["embedding"]) == 768, "embed request on the second model")
    status, models = s.json("/v1/models")
    check(status == 200 and all(m["loaded"] for m in models["data"] if m["roles"]), "both role models loaded")
    code, rss = s.stop()
    check(code == 0, "SIGTERM: exit status 0")
    check(not os.path.exists(s.sock), "SIGTERM: socket file removed")
    write_budget("arc_llm_chat", "arc_llm_tiny_rss_kib", rss)


def test_embed(args):
    s = Server(args, "embed")
    status, doc = s.json("/v1/embeddings", {"input": EMBED_TEXTS})
    check(status == 200 and len(doc.get("data", [])) == len(EMBED_TEXTS), "embeddings for every input")
    status, single = s.json("/v1/embeddings", {"input": EMBED_TEXTS[0], "model": "embed"})
    check(status == 200 and single["data"][0]["embedding"] == doc["data"][0]["embedding"],
          "a string input and the same string in an array give the same vector")
    status, toks = s.json("/v1/tokenize", {"content": TOKENIZE_TEXT})
    s.stop()

    tool = os.path.join(args.tools, "llama-embedding")
    out = llama_lane.run([tool, "-m", args.embed, "-t", "4", "--embd-output-format", "json", "--embd-separator", "|",
                          "-p", "|".join(EMBED_TEXTS)], log_name="arc_llm_embed_ref")[0]
    ref = llama_lane.embeddings(out)
    for text, item, r in zip(EMBED_TEXTS, doc.get("data", []), ref):
        a = item["embedding"]
        norm = sum(x * x for x in a) ** 0.5
        c = llama_lane.cosine(a, r) if len(a) == len(r) else 0.0
        check(abs(norm - 1.0) < 1e-4, f"embedding of {text!r} is L2-normalised ({norm:.6f})")
        check(c >= llama_lane.MIN_COSINE, f"embedding of {text!r}: cosine vs llama-embedding {c:.7f}")

    tool = os.path.join(args.tools, "llama-tokenize")
    ids = llama_lane.run([tool, "-m", args.chat, "-t", "4", "-p", TOKENIZE_TEXT, "--ids", "--log-disable"],
                         log_name="arc_llm_tokenize_ref")[0].strip()
    print(f"tokens: {toks.get('tokens')}")
    check(status == 200 and toks.get("tokens") == json.loads(ids), "tokenize: same IDs as llama-tokenize")


def test_roles(args):
    s = Server(args, "roles")
    before, doc = s.chat_text(CHAT)
    check(doc.get("model_version") == "1.0.0", "chat role served by version 1.0.0")
    write_roles(s.root, {"chat": {"name": "smollm2-135m-instruct", "version": "1.1.0"},
                         "embed": {"name": "nomic-embed-text", "version": "1.5.0"}})
    after, doc = s.chat_text(CHAT)
    check(doc.get("model_version") == "1.1.0", "after the switch the next request uses version 1.1.0")
    check(after == before, "same GGUF: same greedy text")
    status, models = s.json("/v1/models")
    rows = {(m["id"], m["version"]): m for m in models.get("data", [])}
    new = rows.get(("smollm2-135m-instruct", "1.1.0"))
    check(status == 200 and new is not None and new["roles"] == ["chat"] and new["loaded"],
          "/v1/models shows 1.1.0 with role chat, loaded")
    check(("smollm2-135m-instruct", "1.0.0") not in rows, "/v1/models no longer lists 1.0.0 (no role names it)")
    check("no longer has a role; unloaded" in s.stderr(), "1.0.0 was unloaded")
    status, by_name = s.json("/v1/chat/completions", dict(CHAT, model="smollm2-135m-instruct"))
    check(status == 200 and by_name.get("model_version") == "1.1.0", "a model name works as `model` too")
    s.stop()


def test_fit(args):
    s = Server(args, "fit", ["--reserve-mb", str(1 << 30)], stale_socket=True)
    print("ok: started over a stale socket file")
    second = subprocess.run([os.environ["ARC_LANE_RUN"], "arc_llm_fit_second", args.arc_llm, "--root", s.root], stdin=subprocess.DEVNULL, capture_output=True,
                            timeout=60, check=False)
    check(second.returncode == 1 and b"another arc-llm is serving" in second.stderr,
          "a second arc-llm on the same socket refuses to start")
    status, doc = s.json("/v1/chat/completions", CHAT)
    msg = doc.get("error", {}).get("message", "")
    print(f"507 message: {msg}")
    check(status == 507 and "needs 400 MiB" in msg and "MiB of RAM" in msg, "chat: 507 with the numbers")
    status, models = s.json("/v1/models")
    check(status == 200 and not any(m["loaded"] for m in models["data"]), "still serving, nothing loaded")
    status, doc = s.json("/v1/tokenize", {"content": "x"})
    check(status == 507, "every model-loading request gets 507")
    s.stop()


def test_errors(args):
    s = Server(args, "errors")
    cases = [
        ("bad JSON", "/v1/chat/completions", {"raw": b'{"messages": [}'}, 400),
        ("not an object", "/v1/embeddings", {"raw": b'["x"]'}, 400),
        ("no messages", "/v1/chat/completions", {"body": {"max_tokens": 4}}, 400),
        ("bad temperature", "/v1/chat/completions", {"body": dict(CHAT, temperature="hot")}, 400),
        ("unknown role", "/v1/chat/completions", {"body": dict(CHAT, model="expert:astrology")}, 404),
        ("body over 1 MiB", "/v1/embeddings", {"raw": b'{"input":"' + b"a" * (1 << 20) + b'"}'}, 413),
        ("unknown path", "/v1/completions", {"body": {}}, 404),
        ("wrong method", "/v1/models", {"body": {}}, 405),
    ]
    for what, path, kw, want in cases:
        status, doc = s.json(path, kw.get("body"), raw=kw.get("raw"))
        err = doc.get("error", {})
        check(status == want and isinstance(err.get("message"), str) and isinstance(err.get("type"), str),
              f"{what}: {want} with an error object (got {status})")
    status, _ = s.json("/v1/models")
    check(status == 200, "still serving after the errors")
    code, rss = s.stop()
    check(code == 0, "SIGTERM: exit status 0")
    write_budget("arc_llm_errors", "arc_llm_idle_rss_kib", rss)


def drop_mid_stream(server):
    """Start a long streamed chat, read the first content event, hang up. Returns True if it got one."""
    body = json.dumps(dict(CHAT, stream=True, max_tokens=400)).encode()
    head = (f"POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n\r\n").encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
        c.settimeout(120)
        c.connect(server.sock)
        c.sendall(head + body)
        got = b""
        while got.count(b"data: ") < 2:
            chunk = c.recv(4096)
            if not chunk:
                return False
            got += chunk
    return True


def test_clients(args):
    s = Server(args, "clients")
    want, _ = s.chat_text(CHAT)
    body = json.dumps(CHAT).encode()
    cmd = ["curl", "-sS", "--unix-socket", s.sock, "--max-time", "120", "--data-binary", "@-",
           "http://localhost/v1/chat/completions"]
    procs = [subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE) for _ in range(2)]
    outs = [p.communicate(body)[0] for p in procs]
    texts = [json.loads(o)["choices"][0]["message"]["content"] if o else None for o in outs]
    check(texts == [want, want], "two clients at once: both get the greedy answer")
    check(drop_mid_stream(s), "dropping client got the stream started")
    after, _ = s.chat_text(CHAT)
    check(after == want, "server still serves after a client dropped mid-stream")
    log = s.stderr()
    cancelled = [line for line in log.splitlines() if "cancelled after" in line]
    print("\n".join(cancelled))
    check(len(cancelled) == 1 and int(cancelled[0].split()[-2]) < 400, "the dropped stream stopped early")
    code, _ = s.stop()
    check(code == 0, "SIGTERM: exit status 0")


TESTS = {"chat": test_chat, "embed": test_embed, "roles": test_roles, "fit": test_fit, "errors": test_errors,
         "clients": test_clients}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", choices=sorted(TESTS))
    ap.add_argument("--arc-llm", required=True)
    ap.add_argument("--tools", required=True)
    ap.add_argument("--chat", required=True)
    ap.add_argument("--embed", required=True)
    args = ap.parse_args()
    TESTS[args.test](args)
    print(f"arc_llm_{args.test}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
