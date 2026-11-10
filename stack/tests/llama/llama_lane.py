#!/usr/bin/env python3
"""Lane tests for the llama.cpp port (M2-03): our static musl build against the reference build.

    llama_lane.py <test> --ours DIR --ref DIR --chat GGUF --embed GGUF

DIR holds llama-completion, llama-embedding and llama-tokenize. <test> is one of:
    greedy    32 greedy tokens from both builds; stdout identical. Writes the
              llama_tiny_rss_kib budget (peak RSS of our run) to $ARC_LANE_BUDGET_DIR/llama.budget.log.
    nommap    the same with the model read into anonymous memory (-lm none, the
              no-mmap path at this tag) instead of memory-mapped; same text as the reference.
    embed     embeddings of two texts; cosine similarity of ours vs reference >= 0.9999 each.
    tokenize  token IDs of a UTF-8 string with degree, micro and plus-minus signs identical.

This is a driver: it runs untraced. Our binaries go through $ARC_LANE_RUN (tools/lane/run.sh),
so they are traced for the syscall check; the reference build runs directly (it never ships).
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

PROMPT = "Water boils at"
GREEDY = ["-t", "4", "-n", "32", "--temp", "0", "--seed", "1", "-no-cnv", "-p", PROMPT]
EMBED_TEXTS = ["search_query: how to purify water",
               "search_document: Boil water for one minute to make it safe to drink."]
TOKENIZE_TEXT = "Water boils at 100 °C; a 5 µm filter, ± 0.1 µm."
MIN_COSINE = 0.9999


def run(cmd, log_name=None):
    """Run cmd (through $ARC_LANE_RUN as log_name if given). Returns (stdout, peak RSS in KiB).

    The peak RSS is ru_maxrss of this child and the processes it waited for (strace's tracee
    when traced), read with wait4 so earlier runs don't count."""
    if log_name is not None:
        cmd = [os.environ["ARC_LANE_RUN"], log_name] + cmd
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        _, status, usage = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        stdout = out.read().decode("utf-8", errors="replace")
        if p.returncode != 0:
            sys.stderr.write(err.read().decode("utf-8", errors="replace")[-4000:])
            raise SystemExit(f"FAIL: {' '.join(cmd)} exited {p.returncode}")
    return stdout, usage.ru_maxrss


def first_difference(a, b):
    n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    return f"first difference at character {n}:\n  ours: {a[n:n + 80]!r}\n  ref:  {b[n:n + 80]!r}"


def compare_greedy(args, name, extra):
    tool = "llama-completion"
    cmd = ["-m", args.chat] + GREEDY + extra
    ours, rss = run([os.path.join(args.ours, tool)] + cmd, log_name=name)
    ref, _ = run([os.path.join(args.ref, tool)] + cmd)
    print(f"ours: {ours!r}")
    if ours != ref:
        print(f"FAIL: {name}: greedy text differs from the reference build; {first_difference(ours, ref)}")
        return 1, rss
    if PROMPT not in ours or len(ours) <= len(PROMPT):
        print(f"FAIL: {name}: no generated text")
        return 1, rss
    print(f"ok: {name}: 32 greedy tokens identical to the reference build")
    return 0, rss


def test_greedy(args):
    failed, rss = compare_greedy(args, "llama_greedy", [])
    budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
    if budget_dir:
        with open(os.path.join(budget_dir, "llama.budget.log"), "w") as f:
            f.write(f"ARC_TEST budget llama_tiny_rss_kib {rss}\n")
    print(f"llama_tiny_rss_kib = {rss}")
    return failed


def test_nommap(args):
    return compare_greedy(args, "llama_nommap", ["-lm", "none"])[0]


def embeddings(stdout):
    data = json.loads(stdout[stdout.index("{"):])
    return [item["embedding"] for item in data["data"]]


def cosine(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    return dot / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b)))


def test_embed(args):
    tool = "llama-embedding"
    cmd = ["-m", args.embed, "-t", "4", "--embd-output-format", "json", "--embd-separator", "|",
           "-p", "|".join(EMBED_TEXTS)]
    ours = embeddings(run([os.path.join(args.ours, tool)] + cmd, log_name="llama_embed")[0])
    ref = embeddings(run([os.path.join(args.ref, tool)] + cmd)[0])
    failed = 0
    if len(ours) != len(EMBED_TEXTS) or len(ref) != len(EMBED_TEXTS):
        print(f"FAIL: expected {len(EMBED_TEXTS)} embeddings, got {len(ours)} (ours) and {len(ref)} (ref)")
        return 1
    for text, a, b in zip(EMBED_TEXTS, ours, ref):
        c = cosine(a, b) if len(a) == len(b) and a else 0.0
        ok = c >= MIN_COSINE
        failed += not ok
        print(f"{'ok' if ok else 'FAIL'}: embedding of {text!r}: {len(a)} dims, cosine vs reference {c:.7f}")
    return failed


def test_tokenize(args):
    tool = "llama-tokenize"
    # -t 4 as everywhere: without it llama.cpp's common code counts physical cores through
    # /sys/devices/system/cpu/cpu*/topology, which arc_os doesn't provide (arc-llm sets threads too).
    cmd = ["-m", args.chat, "-t", "4", "-p", TOKENIZE_TEXT, "--ids", "--log-disable"]
    ours = run([os.path.join(args.ours, tool)] + cmd, log_name="llama_tokenize")[0].strip()
    ref = run([os.path.join(args.ref, tool)] + cmd)[0].strip()
    print(f"ours: {ours}")
    if ours != ref or not ours.startswith("[") or ours == "[]":
        print(f"FAIL: token IDs differ from the reference build or are empty\n  ref:  {ref}")
        return 1
    print("ok: token IDs identical to the reference build")
    return 0


TESTS = {"greedy": test_greedy, "nommap": test_nommap, "embed": test_embed, "tokenize": test_tokenize}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", choices=sorted(TESTS))
    ap.add_argument("--ours", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--chat", required=True)
    ap.add_argument("--embed", required=True)
    args = ap.parse_args()
    return 1 if TESTS[args.test](args) else 0


if __name__ == "__main__":
    sys.exit(main())
