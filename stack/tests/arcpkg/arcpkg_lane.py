#!/usr/bin/env python3
"""lane.arcpkg_* (M5-02): the arcpkg tool in a temporary root. Fixture packages are built here, untraced:
Python tarfile in ustar format, compressed by the ported zstd CLI (no --long). arcpkg itself runs through
$ARC_LANE_RUN.

  install  a program, a model (tiny GGUF stand-in) and a pack install; verify passes; list; the installed
           manifests (database entries, model.json, the pack's manifest.json) pass tests/schemas/validate.py;
           the same version again is a no-op; a newer program replaces the older one and an older one is
           refused; requires.packages is enforced; a corrupted or deleted installed file fails verify by
           name; remove (a symlink planted in the tree is removed, never followed); leftover .tmp-*
           directories are removed at start.
  refuse   wrong hash, wrong size, missing file, extra file, `../`, an absolute path, a symlink, a hard
           link, a pax header, manifest.json not first, a file outside files/, a cut-short package, a
           --long frame (window over 8 MiB), a bad manifest: each refused, leaving the root exactly as it
           was (no temp directory, database unchanged).
  models   a model larger than RAM is refused (installed with --force and a warning); two versions side
           by side; `role` switches between them and keeps other roles; removing the role's version is
           refused (allowed with --force); a role the model doesn't list, or an uninstalled model, refused.
  big      a 200 MiB pack installs and verifies; peak RSS of the install (through lane_rss, so the driver's
           own RSS isn't counted) goes to arcpkg_big.budget.log as arcpkg_install_rss_kib.
"""

import argparse
import glob
import hashlib
import io
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

failures = 0
runs = 0
BIG_SIZE = 200 << 20


def check(ok, what):
    global failures
    print(f"{'ok' if ok else 'FAIL'}: {what}")
    failures += not ok
    return ok


def arcpkg(args, root, *argv):
    """Run `arcpkg --root root argv...` through the lane runner: (exit code, stdout, stderr)."""
    global runs
    runs += 1
    p = subprocess.run([os.environ["ARC_LANE_RUN"], f"arcpkg_{args.test}{runs}", args.arcpkg, "--root", root, *argv],
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       errors="replace")
    sys.stdout.write(p.stdout)
    sys.stdout.write(p.stderr)
    return p.returncode, p.stdout, p.stderr


# ---- fixture packages --------------------------------------------------------------------------------


class Payload:
    """A file's content: bytes, or `size` pseudo-random bytes from `seed` (streamed, never held whole)."""

    def __init__(self, data=None, size=0, seed=0, mode=0o644):
        self.data, self.size, self.seed, self.mode = data, len(data) if data is not None else size, seed, mode

    def reader(self):
        if self.data is not None:
            return io.BytesIO(self.data)
        return Stream(self.size, self.seed)

    def sha256(self):
        h, r = hashlib.sha256(), self.reader()
        while block := r.read(1 << 20):
            h.update(block)
        return h.hexdigest()


class Stream:
    def __init__(self, size, seed):
        self.left, self.rng = size, random.Random(seed)

    def read(self, n=-1):
        n = self.left if n < 0 else min(n, self.left)
        self.left -= n
        return self.rng.randbytes(n)


def file_list(files):
    return [{"path": p, "size": f.size, "sha256": f.sha256()} for p, f in sorted(files.items())]


def manifest(kind, name, version, files, ram_mb=0, packages=None, model_ram=None, roles=("chat",)):
    """A package manifest (schemas/arcpkg.schema.json) for `files` {path: Payload}; adds model.json or the
    pack's manifest.json to files first, as the package builders do."""
    area = {"program": "apps", "model": "models", "pack": "packs"}[kind]
    m = {"schema_version": 1, "kind": kind, "name": name, "version": version,
         "arch": "x86_64" if kind == "program" else "any",
         "requires": {"ram_mb": ram_mb, "disk_bytes": sum(f.size for f in files.values())},
         "install_root": f"/{area}/{name}", "license": "MIT", "source": "stack/tests/arcpkg (generated)"}
    if packages:
        m["requires"]["packages"] = packages
    if kind == "program":
        m["cpu_level"] = "x86-64-v3"
        m["provides"] = [name]
    elif kind == "model":
        gguf = files["model.gguf"]
        m["model"] = {"schema_version": 1, "name": name, "version": version, "roles": list(roles),
                      "file": "model.gguf", "sha256": gguf.sha256(), "parameter_count": 1000,
                      "quantization": "Q8_0", "ram_mb": model_ram if model_ram is not None else ram_mb,
                      "context_length": 512, "chat_template": "{{ messages }}", "license": "MIT",
                      "source": "stack/tests/arcpkg (generated)"}
        files["model.json"] = Payload(json.dumps(m["model"], indent=2).encode())
    else:
        m["pack"] = {"schema_version": 1, "name": name, "version": version, "title": f"{name} lane test pack",
                     "category": "medical", "priority": 10, "size_bytes": sum(f.size for f in files.values()),
                     "license": "CC0-1.0", "source": "stack/tests/arcpkg (generated)",
                     "checksums": {p: f.sha256() for p, f in sorted(files.items())}, "provides": ["fts"],
                     "chunking": {"tokenizer": "words", "target_tokens": 400, "max_tokens": 500,
                                  "overlap_tokens": 50},
                     "document_count": 1, "chunk_count": 1, "domain_tags": ["general"]}
        files["manifest.json"] = Payload(json.dumps(m["pack"], indent=2).encode())
    m["files"] = file_list(files)
    return m


def member(name, size=0, kind=tarfile.REGTYPE, mode=0o644, link=""):
    info = tarfile.TarInfo(name)
    info.size, info.type, info.mode, info.linkname, info.mtime = size, kind, mode, link, 1700000000
    return info


def build(path, man, files, extra=(), first=None, zstd_args=(), tar_format=tarfile.USTAR_FORMAT):
    """Write `path`: manifest.json (or `first`), files/ and each file, then `extra` [(TarInfo, bytes)],
    as ustar through the zstd CLI. Returns path."""
    with open(path, "wb") as out:
        z = subprocess.Popen([args_g.zstd, "-q", "-c", "-3", *zstd_args], stdin=subprocess.PIPE, stdout=out)
        with tarfile.open(fileobj=z.stdin, mode="w|", format=tar_format) as tar:
            members = [first] if first else []
            if not first:
                text = json.dumps(man, indent=2).encode()
                members.append((member("manifest.json", len(text)), io.BytesIO(text)))
            members.append((member("files", kind=tarfile.DIRTYPE, mode=0o755), None))
            for p, f in sorted(files.items()):
                members.append((member(f"files/{p}", f.size, mode=f.mode), f.reader()))
            members += [(info, io.BytesIO(data) if data is not None else None) for info, data in extra]
            for info, data in members:
                tar.addfile(info, data)
        z.stdin.close()
        check(z.wait() == 0, f"zstd compressed {os.path.basename(path)}")
    return path


def program(tmp, version, exe=b"\x7fELF lane test program\n", **kw):
    files = {"bin/hello": Payload(exe + version.encode(), mode=0o755), "share/hello/README": Payload(b"hello\n")}
    m = manifest("program", "hello", version, files, ram_mb=16, **kw)
    return build(os.path.join(tmp, f"hello-{version}.arcpkg"), m, files), m, files


def model(tmp, version, ram_mb=64, name="tiny-model", roles=("chat",)):
    files = {"model.gguf": Payload(b"GGUF\x03\x00\x00\x00" + bytes(range(256)) * 16 + version.encode())}
    m = manifest("model", name, version, files, ram_mb=ram_mb, roles=roles)
    return build(os.path.join(tmp, f"{name}-{version}.arcpkg"), m, files), m, files


def pack(tmp, version, packages=None, big=False):
    files = {"content/docs.zst": Payload(b"docs " * 100), "chunks/chunks.idx": Payload(b"\x00" * 4096),
             "fts/index.sqlite": Payload(b"SQLite format 3\x00" + b"\x01" * 1000)}
    if big:
        files["vectors/big.bin"] = Payload(size=BIG_SIZE, seed=7)
    m = manifest("pack", "tiny-pack", version, files, packages=packages)
    return build(os.path.join(tmp, f"tiny-pack-{version}.arcpkg"), m, files), m, files


# ---- checks on the root ------------------------------------------------------------------------------


def snapshot(root):
    """Every path under root with its type, mode and content hash: equal snapshots mean no trace."""
    out = []
    for dirpath, dirs, names in os.walk(root):
        for d in dirs:
            out.append((os.path.relpath(os.path.join(dirpath, d), root), "dir"))
        for n in names:
            p = os.path.join(dirpath, n)
            with open(p, "rb") as f:
                out.append((os.path.relpath(p, root), hashlib.sha256(f.read()).hexdigest(), os.stat(p).st_mode))
    return sorted(out, key=str)


def db(root, name):
    path = os.path.join(root, "var/lib/arcpkg", f"{name}.json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def db_versions(root, name):
    d = db(root, name)
    return [v["version"] for v in d["versions"]] if d else []


def validate(args, root, tmp):
    """Every database manifest, model.json, pack manifest.json and roles.json under root, through validate.py."""
    out = os.path.join(tmp, "validate")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    paths = []
    for name in sorted(os.listdir(os.path.join(root, "var/lib/arcpkg"))):
        for v in db(root, name[:-5])["versions"]:
            paths.append(os.path.join(out, f"arcpkg.{name[:-5]}-{v['version']}.json"))
            with open(paths[-1], "w") as f:
                json.dump(v["manifest"], f)
    for kind, pattern in (("model", "models/*/*/model.json"), ("pack", "packs/*/*/manifest.json"),
                          ("model-roles", "models/roles.json")):
        for i, p in enumerate(sorted(glob.glob(os.path.join(root, pattern)))):
            paths.append(os.path.join(out, f"{kind}.installed{i}.json"))
            shutil.copy(p, paths[-1])
    p = subprocess.run([sys.executable, args.validate, *paths], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    sys.stdout.write(p.stdout)
    return p.returncode == 0 and len(paths) > 0, len(paths)


# ---- tests -------------------------------------------------------------------------------------------


def test_install(args, root, tmp):
    prog1, _, _ = program(tmp, "1.0.0")
    mod, _, _ = model(tmp, "1.0.0")
    pk, _, _ = pack(tmp, "1.0.0", packages=[{"name": "tiny-model", "min_version": "1.0.0"}])

    before = snapshot(root)
    code, _, err = arcpkg(args, root, "install", pk)
    check(code == 1 and "needs package tiny-model 1.0.0" in err and snapshot(root) == before,
          "a pack whose required model isn't installed is refused, no trace")
    for path, what in ((prog1, "program"), (mod, "model"), (pk, "pack")):
        code, out, _ = arcpkg(args, root, "install", path)
        check(code == 0 and "installed" in out, f"install the {what}")
    hello = os.path.join(root, "apps/hello/1.0.0/bin/hello")
    check(os.path.isfile(hello) and os.stat(hello).st_mode & 0o111 and
          not os.stat(os.path.join(root, "apps/hello/1.0.0/share/hello/README")).st_mode & 0o111,
          "program files installed with their modes (bin/hello executable, README not)")
    check(os.path.isfile(os.path.join(root, "models/tiny-model/1.0.0/model.json")) and
          os.path.isfile(os.path.join(root, "packs/tiny-pack/1.0.0/fts/index.sqlite")),
          "model and pack installed under <install_root>/<version>/")
    code, out, _ = arcpkg(args, root, "verify")
    check(code == 0 and "verified" in out, "verify passes after install")
    code, out, _ = arcpkg(args, root, "list")
    rows = [line.split("\t") for line in out.splitlines()]
    check(code == 0 and [r[:3] for r in rows] == [["hello", "program", "1.0.0"], ["tiny-model", "model", "1.0.0"],
                                                 ["tiny-pack", "pack", "1.0.0"]], "list: name, kind, versions")
    ok, n = validate(args, root, tmp)
    check(ok, f"the {n} installed manifests pass tests/schemas/validate.py")

    before = snapshot(root)
    code, out, _ = arcpkg(args, root, "install", prog1)
    check(code == 0 and "already installed" in out and snapshot(root) == before,
          "reinstalling the same version is a no-op")

    prog2, _, _ = program(tmp, "1.1.0")
    code, _, _ = arcpkg(args, root, "install", prog2)
    check(code == 0 and db_versions(root, "hello") == ["1.1.0"] and
          not os.path.exists(os.path.join(root, "apps/hello/1.0.0")) and
          os.path.isfile(os.path.join(root, "apps/hello/1.1.0/bin/hello")),
          "a newer program replaces the older version")
    before = snapshot(root)
    code, _, err = arcpkg(args, root, "install", prog1)
    check(code == 1 and "newer" in err and snapshot(root) == before, "an older program than the installed one is refused")

    with open(os.path.join(root, "packs/tiny-pack/1.0.0/content/docs.zst"), "r+b") as f:
        f.write(b"X")
    os.remove(os.path.join(root, "apps/hello/1.1.0/share/hello/README"))
    code, out, _ = arcpkg(args, root, "verify")
    check(code == 1 and "tiny-pack 1.0.0: content/docs.zst: SHA-256 differs" in out and
          "hello 1.1.0: share/hello/README: missing" in out, "a corrupted and a deleted file fail verify by name")
    code, out, _ = arcpkg(args, root, "verify", "tiny-model")
    check(code == 0 and "verified" in out, "verify NAME checks only that package")
    code, _, err = arcpkg(args, root, "verify", "nothing-here")
    check(code == 1 and "not installed" in err, "verify of a package that isn't installed fails")

    outside = os.path.join(tmp, "outside")
    os.makedirs(outside)
    open(os.path.join(outside, "keep"), "w").close()
    os.symlink(outside, os.path.join(root, "apps/hello/1.1.0/share/link"))
    code, out, _ = arcpkg(args, root, "remove", "hello")
    check(code == 0 and db(root, "hello") is None and not os.listdir(os.path.join(root, "apps/hello")),
          "remove a program: its versions and database entry are gone")
    check(os.path.isfile(os.path.join(outside, "keep")), "remove deletes a symlink in the tree, not what it points at")
    code, _, err = arcpkg(args, root, "remove", "hello")
    check(code == 1 and "not installed" in err, "remove of a package that isn't installed fails")

    leftover = os.path.join(root, "packs/tiny-pack/.tmp-tiny-pack-9.9.9/content")
    os.makedirs(leftover)
    open(os.path.join(leftover, "half"), "w").close()
    code, out, err = arcpkg(args, root, "list")
    check(code == 0 and not os.path.exists(os.path.dirname(leftover)) and "interrupted install" in err,
          "a .tmp-* directory left by an interrupted install is removed at start")
    for bad in (["frob"], ["install"], ["remove", "../x"], ["verify", "a", "b"], ["role", "chat"], ["--bogus", "list"]):
        code, _, _ = arcpkg(args, root, *bad)
        check(code == 2, f"usage error: {' '.join(bad)}")


def tamper(tmp, label, **kw):
    """A program package with one thing wrong, from build() keywords or a changed manifest."""
    files = {"bin/hello": Payload(b"\x7fELF hello\n", mode=0o755), "doc/README": Payload(b"readme\n")}
    m = manifest("program", "hello", "1.0.0", files)
    files = dict(files)
    edit = kw.pop("edit", None)
    if edit:
        edit(m, files)
    return build(os.path.join(tmp, f"bad-{label}.arcpkg"), m, files, **kw)


def test_refuse(args, root, tmp):
    good, _, _ = model(tmp, "1.0.0")  # something installed, so "database unchanged" means something
    check(arcpkg(args, root, "install", good)[0] == 0, "install a model first")
    text = b"plain text\n"
    cases = [
        ("wrong-hash", "SHA-256 differs", {"edit": lambda m, f: m["files"][0].update(sha256="0" * 64)}),
        ("wrong-size", "manifest.json says", {"edit": lambda m, f: m["files"][0].update(size=3)}),
        ("missing-file", "not in the archive", {"edit": lambda m, f: f.pop("doc/README")}),
        ("extra-file", "not in manifest.json", {"extra": [(member("files/doc/extra", len(text)), text)]}),
        ("dotdot", "not a safe relative path", {"extra": [(member("files/../escape", len(text)), text)]}),
        ("absolute", "not a safe relative path", {"extra": [(member("/etc/escape", len(text)), text)]}),
        ("symlink", "is a symlink", {"extra": [(member("files/link", kind=tarfile.SYMTYPE, link="/etc/passwd"),
                                                None)]}),
        ("hardlink", "is a hard link", {"extra": [(member("files/hard", kind=tarfile.LNKTYPE, link="files/bin/hello"),
                                                   None)]}),
        ("pax", "pax extended header", {"extra": [(member("files/" + "p" * 120, len(text)), text)],
                                        "tar_format": tarfile.PAX_FORMAT}),
        ("not-first", "first member must be manifest.json", {"first": (member("files", kind=tarfile.DIRTYPE), None)}),
        ("outside", "outside files/", {"extra": [(member("elsewhere", len(text)), text)]}),
        ("bad-manifest", "install_root", {"edit": lambda m, f: m.update(install_root="/system/hello")}),
        ("no-cpu-level", "cpu_level", {"edit": lambda m, f: m.pop("cpu_level")}),
        ("dup-file", "listed twice", {"edit": lambda m, f: m["files"].append(dict(m["files"][0]))}),
    ]
    for label, message, kw in cases:
        path = tamper(tmp, label, **kw)
        before = snapshot(root)
        code, _, err = arcpkg(args, root, "install", path)
        check(code == 1 and message in err and snapshot(root) == before, f"{label}: refused ({message}), no trace")

    # A frame whose window is over 8 MiB (zstd --long=24 with more than 8 MiB of data).
    files = {"blob": Payload(size=12 << 20, seed=3)}
    m = manifest("program", "hello", "1.0.0", files)
    m["cpu_level"] = "x86-64-v3"
    path = build(os.path.join(tmp, "bad-long.arcpkg"), m, files, zstd_args=("--long=24",))
    before = snapshot(root)
    code, _, err = arcpkg(args, root, "install", path)
    check(code == 1 and "window of at most 8 MiB" in err and snapshot(root) == before, "--long frame refused, no trace")

    path = tamper(tmp, "cut")
    with open(path, "r+b") as f:
        f.truncate(os.path.getsize(path) // 2)
    code, _, err = arcpkg(args, root, "install", path)
    check(code == 1 and "cut short" in err and snapshot(root) == before, "a cut-short package refused, no trace")
    code, _, err = arcpkg(args, root, "install", os.path.join(tmp, "no-such.arcpkg"))
    check(code == 1 and "No such file" in err, "a missing package file reported")


def test_models(args, root, tmp):
    huge, _, _ = model(tmp, "0.1.0", ram_mb=1 << 40, name="huge-model")
    before = snapshot(root)
    code, _, err = arcpkg(args, root, "install", huge)
    check(code == 1 and "this machine has" in err and snapshot(root) == before, "a model larger than RAM is refused")
    code, _, err = arcpkg(args, root, "--force", "install", huge)
    check(code == 0 and "warning" in err and db_versions(root, "huge-model") == ["0.1.0"],
          "--force installs it with a warning")

    v1, _, _ = model(tmp, "1.0.0")
    v2, _, _ = model(tmp, "1.1.0")
    check(arcpkg(args, root, "install", v2)[0] == 0 and arcpkg(args, root, "install", v1)[0] == 0 and
          db_versions(root, "tiny-model") == ["1.0.0", "1.1.0"] and
          os.path.isdir(os.path.join(root, "models/tiny-model/1.0.0")) and
          os.path.isdir(os.path.join(root, "models/tiny-model/1.1.0")), "two model versions installed side by side")

    roles_path = os.path.join(root, "models/roles.json")
    with open(roles_path, "w") as f:
        json.dump({"schema_version": 1, "roles": {"expert:medicine": {"name": "huge-model", "version": "0.1.0"}}}, f)

    def roles():
        with open(roles_path) as f:
            return json.load(f)["roles"]

    code, out, _ = arcpkg(args, root, "role", "chat", "tiny-model", "1.0.0")
    check(code == 0 and roles().get("chat") == {"name": "tiny-model", "version": "1.0.0"} and
          "expert:medicine" in roles(), "role points chat at 1.0.0, other roles kept")
    code, out, _ = arcpkg(args, root, "role", "chat", "tiny-model", "1.1.0")
    check(code == 0 and roles()["chat"]["version"] == "1.1.0", "role switches chat to 1.1.0")
    code, out, _ = arcpkg(args, root, "role")
    check(code == 0 and "chat\ttiny-model\t1.1.0" in out and "expert:medicine\thuge-model\t0.1.0" in out,
          "role shows roles.json")
    for argv, why in ((["role", "embed", "tiny-model", "1.1.0"], "does not list the role"),
                      (["role", "chat", "tiny-model", "3.0.0"], "not installed"),
                      (["role", "writer", "tiny-model", "1.1.0"], "is not a role")):
        before = snapshot(root)
        code, _, err = arcpkg(args, root, *argv)
        check(code == 1 and why in err and snapshot(root) == before, f"{' '.join(argv)}: refused ({why})")
    ok, n = validate(args, root, tmp)
    check(ok, f"the {n} installed manifests and roles.json pass tests/schemas/validate.py")

    before = snapshot(root)
    code, _, err = arcpkg(args, root, "remove", "tiny-model", "1.1.0")
    check(code == 1 and "role chat points at" in err and snapshot(root) == before, "removing the role's version is refused")
    code, _, err = arcpkg(args, root, "remove", "tiny-model")
    check(code == 1 and snapshot(root) == before, "removing every version, one a role uses, is refused")
    code, _, _ = arcpkg(args, root, "remove", "tiny-model", "1.0.0")
    check(code == 0 and db_versions(root, "tiny-model") == ["1.1.0"] and
          not os.path.exists(os.path.join(root, "models/tiny-model/1.0.0")), "removing an unused version works")
    code, _, err = arcpkg(args, root, "--force", "remove", "tiny-model", "1.1.0")
    check(code == 0 and "warning" in err and db(root, "tiny-model") is None, "--force removes the role's version")


def test_big(args, root, tmp):
    path, _, _ = pack(tmp, "1.0.0", big=True)
    global runs
    runs += 1
    p = subprocess.run([os.environ["ARC_LANE_RUN"], "arcpkg_big_install", args.lane_rss, args.arcpkg, "--root", root,
                        "install", path], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True)
    sys.stdout.write(p.stdout + p.stderr)
    m = re.search(r"lane_rss: peak RSS (\d+) KiB", p.stderr)
    check(p.returncode == 0 and m is not None, f"a {BIG_SIZE >> 20} MiB pack installs")
    big = os.path.join(root, "packs/tiny-pack/1.0.0/vectors/big.bin")
    check(os.path.isfile(big) and os.path.getsize(big) == BIG_SIZE, "the big file is in place")
    os.remove(path)
    code, out, _ = arcpkg(args, root, "verify")
    check(code == 0 and "verified" in out, "verify re-hashes it")
    if m:
        print(f"arcpkg install peak RSS: {m.group(1)} KiB")
        budget_dir = os.environ.get("ARC_LANE_BUDGET_DIR")
        if budget_dir:
            with open(os.path.join(budget_dir, "arcpkg_big.budget.log"), "w") as f:
                f.write(f"ARC_TEST budget arcpkg_install_rss_kib {m.group(1)}\n")


TESTS = {"install": test_install, "refuse": test_refuse, "models": test_models, "big": test_big}
args_g = None


def main():
    global args_g
    ap = argparse.ArgumentParser()
    ap.add_argument("test", choices=sorted(TESTS))
    ap.add_argument("--arcpkg", required=True)
    ap.add_argument("--zstd", required=True)
    ap.add_argument("--lane-rss", required=True)
    ap.add_argument("--validate", required=True)
    args_g = args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix=f"arcpkg-{args.test}-")
    try:
        root = os.path.join(tmp, "root")
        os.makedirs(root)
        TESTS[args.test](args, root, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"arcpkg_{args.test}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
