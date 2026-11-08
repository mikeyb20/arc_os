#!/usr/bin/env python3
"""Archive of outside components (M0-03, knowledge-os.md §3.4).

third_party/SOURCES.lock pins every outside component by version and SHA-256.
Copies live as release assets on Michael's GitHub, so no build or session needs a
third-party server once a component is archived.

  archive.py mirror   Used by .github/workflows/archive.yml. For each component
                      without an archive URL: download it from upstream, check its
                      SHA-256 (a missing hash is recorded, a mismatch fails), upload
                      it to today's release (archive-YYYYMMDD) and record the
                      archive URL in the lock file. Needs `gh` and GH_TOKEN.
  archive.py fetch    Used by tools/fetch-archived.sh. Download named components
                      from the archive only, verify SHA-256, unpack into
                      third_party/src/<name>/.
  archive.py check    Validate the lock file (CI, no network).

Downloads use curl, which honours the proxy and CA settings of cloud sessions.
"""

import argparse
import datetime
import gzip
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOCK = os.path.join(ROOT, "third_party", "SOURCES.lock")
SPLIT_BYTES = 2000 * 1024 * 1024          # release assets must stay under 2 GiB
SUFFIXES = (".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".zip")
KINDS = ("source", "binary", "reference", "test-model")
SHIPS = ("build-host", "install-key", "inside-arc_os", "tests-only")
REQUIRED = ("name", "version", "kind", "license", "ships", "upstream_url", "sha256", "archive_url")


def log(msg):
    print(f"archive: {msg}", flush=True)


def load_lock(path):
    with open(path) as f:
        return json.load(f)


def save_lock(path, lock):
    with open(path, "w") as f:
        json.dump(lock, f, indent=2)
        f.write("\n")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def curl(url, dest):
    """Download url to dest. Returns True on success."""
    res = subprocess.run(["curl", "-fsSL", "--retry", "4", "--retry-delay", "2",
                          "--connect-timeout", "30", "-o", dest, url])
    return res.returncode == 0


def suffix_of(url):
    base = url.split("?")[0].rsplit("/", 1)[-1]
    for s in SUFFIXES:
        if base.endswith(s):
            return s
    ext = os.path.splitext(base)[1]
    return ext if ext else ".bin"


def asset_name(comp):
    if comp.get("files"):
        return f"{comp['name']}-{comp['version']}.tar.gz"
    return f"{comp['name']}-{comp['version']}{suffix_of(comp['upstream_url'])}"


# --- check ---

def check(lock):
    """Return a list of problems in the lock file (no network)."""
    problems = []
    if lock.get("schema_version") != 1:
        problems.append("schema_version must be 1")
    seen = set()
    for c in lock.get("components", []):
        name = c.get("name", "?")
        for k in REQUIRED:
            if k not in c:
                problems.append(f"{name}: missing field '{k}'")
        if name in seen:
            problems.append(f"{name}: duplicate name")
        seen.add(name)
        if c.get("kind") not in KINDS:
            problems.append(f"{name}: kind must be one of {KINDS}")
        if c.get("ships") not in SHIPS:
            problems.append(f"{name}: ships must be one of {SHIPS}")
        h = c.get("sha256", "")
        if h and (len(h) != 64 or any(ch not in "0123456789abcdef" for ch in h)):
            problems.append(f"{name}: sha256 is not 64 lowercase hex digits")
        for f in c.get("files", []):
            if not f.get("path") or not f.get("url"):
                problems.append(f"{name}: every file needs path and url")
    return problems


# --- mirror (workflow side) ---

def build_bundle(comp, work):
    """Download a multi-file component and pack it as a reproducible tar.gz.
    Returns (path, problems)."""
    problems = []
    blobs = []
    for f in comp["files"]:
        dest = os.path.join(work, "file")
        if not curl(f["url"], dest):
            problems.append(f"{comp['name']}: download failed: {f['url']}")
            continue
        got = sha256_file(dest)
        if f.get("sha256") and f["sha256"] != got:
            problems.append(f"{comp['name']}: {f['path']}: sha256 mismatch "
                            f"(lock {f['sha256']}, got {got})")
            continue
        f["sha256"] = got
        with open(dest, "rb") as fh:
            blobs.append((f["path"], fh.read()))
    if problems:
        return None, problems

    # Fixed order, owner and times, and gzip without a timestamp: the same
    # files always give the same bytes, so the hash can be re-checked.
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for path, data in sorted(blobs):
            info = tarfile.TarInfo(f"{comp['name']}-{comp['version']}/{path}")
            info.size = len(data)
            info.mtime = 0
            info.mode = 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    out = os.path.join(work, asset_name(comp))
    with open(out, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
            gz.write(raw.getvalue())
    return out, []


def existing_assets(repo, tag):
    res = subprocess.run(["gh", "release", "view", tag, "--repo", repo, "--json", "assets",
                          "--jq", ".assets[].name"], capture_output=True, text=True)
    if res.returncode != 0:
        return None
    return set(res.stdout.split())


def ensure_release(repo, tag, target):
    if existing_assets(repo, tag) is not None:
        return True
    cmd = ["gh", "release", "create", tag, "--repo", repo, "--title", tag,
           "--notes", "Archived outside components (third_party/SOURCES.lock, M0-03). "
                      "Assets are never replaced in place."]
    if target:
        cmd += ["--target", target]
    return subprocess.run(cmd).returncode == 0


def upload(repo, tag, paths):
    have = existing_assets(repo, tag) or set()
    todo = [p for p in paths if os.path.basename(p) not in have]
    if not todo:
        return True
    return subprocess.run(["gh", "release", "upload", tag, "--repo", repo] + todo).returncode == 0


def split_file(path):
    """Split a file into .partNN pieces under SPLIT_BYTES. Returns the piece paths."""
    parts = []
    with open(path, "rb") as f:
        n = 0
        while True:
            chunk = f.read(SPLIT_BYTES)
            if not chunk:
                break
            part = f"{path}.part{n:02d}"
            with open(part, "wb") as out:
                out.write(chunk)
            parts.append(part)
            n += 1
    return parts


def mirror(args):
    lock = load_lock(args.lock)
    problems = check(lock)
    if problems:
        for p in problems:
            log(p)
        return 1

    tag = args.tag or "archive-" + datetime.date.today().strftime("%Y%m%d")
    base = f"https://github.com/{args.repo}/releases/download/{tag}"
    work = os.path.join(ROOT, "build", "archive-work")
    os.makedirs(work, exist_ok=True)
    failed, done, skipped = [], [], []
    release_ready = False

    for comp in lock["components"]:
        name = comp["name"]
        if args.only and name not in args.only:
            continue
        if comp["archive_url"] and comp["sha256"]:
            skipped.append(name)
            continue
        log(f"{name} {comp['version']}: downloading")
        if comp.get("files"):
            path, errs = build_bundle(comp, work)
            if errs:
                failed.extend(errs)
                continue
            got = sha256_file(path)
            if comp["sha256"] and comp["sha256"] != got:
                failed.append(f"{name}: bundle sha256 mismatch (lock {comp['sha256']}, got {got})")
                continue
        else:
            path = os.path.join(work, asset_name(comp))
            if not curl(comp["upstream_url"], path):
                failed.append(f"{name}: download failed: {comp['upstream_url']}")
                continue
            got = sha256_file(path)
            if comp["sha256"] and comp["sha256"] != got:
                failed.append(f"{name}: sha256 mismatch (lock {comp['sha256']}, got {got})")
                os.remove(path)
                continue

        if not release_ready:
            release_ready = ensure_release(args.repo, tag, args.target)
            if not release_ready:
                failed.append(f"cannot create release {tag}")
                break
        pieces = split_file(path) if os.path.getsize(path) > SPLIT_BYTES else [path]
        if not upload(args.repo, tag, pieces):
            failed.append(f"{name}: upload failed")
            continue
        comp["sha256"] = got
        if len(pieces) > 1:
            comp["archive_parts"] = [f"{base}/{os.path.basename(p)}" for p in pieces]
        comp["archive_url"] = f"{base}/{os.path.basename(path)}"
        done.append(name)
        for p in set(pieces + [path]):
            os.remove(p)
        save_lock(args.lock, lock)     # keep progress if a later entry fails

    save_lock(args.lock, lock)
    log(f"archived {len(done)}: {', '.join(done) or '-'}")
    log(f"already archived {len(skipped)}")
    for f in failed:
        log(f"FAILED {f}")
    return 1 if failed else 0


# --- fetch (session side) ---

def unpack(path, dest, comp):
    """Unpack into dest, dropping a single top-level directory."""
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    url = comp["upstream_url"] if not comp.get("files") else "x.tar.gz"
    suffix = suffix_of(url)
    if suffix == ".zip":
        with zipfile.ZipFile(path) as z:
            z.extractall(tmp)
    elif suffix in SUFFIXES:
        with tarfile.open(path) as t:
            try:
                t.extractall(tmp, filter="data")     # Python 3.12+: refuse unsafe paths
            except TypeError:
                t.extractall(tmp)
    else:
        shutil.copy(path, os.path.join(tmp, comp["upstream_url"].split("?")[0].rsplit("/", 1)[-1]))
    entries = os.listdir(tmp)
    src = os.path.join(tmp, entries[0]) if len(entries) == 1 and \
        os.path.isdir(os.path.join(tmp, entries[0])) else tmp
    shutil.rmtree(dest, ignore_errors=True)
    shutil.move(src, dest)
    shutil.rmtree(tmp, ignore_errors=True)


def fetch(args):
    lock = load_lock(args.lock)
    comps = {c["name"]: c for c in lock["components"]}
    cache = os.path.join(ROOT, "third_party", "cache")
    os.makedirs(cache, exist_ok=True)
    rc = 0
    for name in args.names:
        comp = comps.get(name)
        if comp is None:
            log(f"{name}: not in {os.path.relpath(args.lock, ROOT)}")
            rc = 1
            continue
        if not comp["archive_url"] or not comp["sha256"]:
            log(f"{name}: not archived yet (run the archive workflow)")
            rc = 1
            continue
        path = os.path.join(cache, comp["archive_url"].rsplit("/", 1)[-1])
        if not (os.path.isfile(path) and sha256_file(path) == comp["sha256"]):
            urls = comp.get("archive_parts") or [comp["archive_url"]]
            ok = True
            with open(path, "wb") as out:
                for i, u in enumerate(urls):
                    piece = f"{path}.dl{i}"
                    if not curl(u, piece):
                        log(f"{name}: download failed: {u}")
                        ok = False
                        break
                    with open(piece, "rb") as p:
                        shutil.copyfileobj(p, out)
                    os.remove(piece)
            if not ok:
                os.remove(path)
                rc = 1
                continue
            got = sha256_file(path)
            if got != comp["sha256"]:
                log(f"{name}: sha256 mismatch (lock {comp['sha256']}, got {got})")
                os.remove(path)
                rc = 1
                continue
        if getattr(args, "cache_only", False):
            log(f"{name} {comp['version']}: verified, kept as {os.path.relpath(path, ROOT)}")
            continue
        dest = os.path.join(args.dest, name)
        unpack(path, dest, comp)
        log(f"{name} {comp['version']}: verified, unpacked to {os.path.relpath(dest, ROOT)}")
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lock", default=DEFAULT_LOCK)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mirror")
    m.add_argument("--repo", required=True, help="owner/name that holds the releases")
    m.add_argument("--tag", help="release tag (default archive-YYYYMMDD)")
    m.add_argument("--target", help="commit for a new release tag")
    m.add_argument("--only", nargs="*", help="archive only these components")
    f = sub.add_parser("fetch")
    f.add_argument("names", nargs="+")
    f.add_argument("--dest", default=os.path.join(ROOT, "third_party", "src"))
    f.add_argument("--cache-only", action="store_true",
                   help="verify into third_party/cache/ but don't unpack (tools that want the tarball)")
    sub.add_parser("check")
    args = ap.parse_args()

    if args.cmd == "mirror":
        return mirror(args)
    if args.cmd == "fetch":
        return fetch(args)
    problems = check(load_lock(args.lock))
    for p in problems:
        log(p)
    if not problems:
        log("lock file OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
