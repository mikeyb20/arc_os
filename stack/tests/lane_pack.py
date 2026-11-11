"""Pack helpers the Python lane drivers share (M3-02, M3-06a): header counts and manifest.json.

The manifest is written as the pack pipeline will (M3-04): schemas/pack.schema.json fields, SHA-256
checksums of every other file in the pack, and document and chunk counts read from the file headers.
"""

import hashlib
import json
import os
import struct

HEADER = 4096


def header_count(path):
    """The `count` field of a pack file's header (docs/formats/pack-v1.md)."""
    with open(path, "rb") as f:
        return struct.unpack_from("<Q", f.read(24), 16)[0]


def write_manifest(pack, name, version, tags, /, **override):
    """Write `pack`/manifest.json for the pack files in `pack`; `override` replaces any field (even name)."""
    checksums, size = {}, 0
    for dirpath, _, files in os.walk(pack):
        for fname in sorted(files):
            path = os.path.join(dirpath, fname)
            rel = os.path.relpath(path, pack)
            if rel == "manifest.json":
                continue
            with open(path, "rb") as f:
                data = f.read()
            checksums[rel] = hashlib.sha256(data).hexdigest()
            size += len(data)
    manifest = {
        "schema_version": 1, "name": name, "version": version, "title": f"{name} lane test pack",
        "category": "medical", "priority": 10, "size_bytes": size, "license": "CC0-1.0",
        "source": "stack/tests (generated)", "checksums": checksums, "provides": ["fts"],
        "chunking": {"tokenizer": "words", "target_tokens": 400, "max_tokens": 500, "overlap_tokens": 50},
        "document_count": header_count(os.path.join(pack, "content/docs.idx")),
        "chunk_count": header_count(os.path.join(pack, "chunks/chunks.idx")),
        "domain_tags": tags,
    }
    manifest.update(override)
    with open(os.path.join(pack, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest
