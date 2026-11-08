#!/usr/bin/env python3
"""Validate the arc_os manifest schemas and their examples (M0-07, ADR-0010/0011/0012).

Checks that every schema in schemas/ is a valid JSON Schema (draft 2020-12),
that every schemas/examples/<schema>.valid*.json passes, and that every
<schema>.invalid-<reason>.json fails for the reason its name gives (EXPECTED
below), not some other mistake. Also checks what a schema cannot say: an
.arcpkg manifest's `model` or `pack` object has the package's name and version.

    python3 tests/schemas/validate.py            # exit 0 = all good
    python3 tests/schemas/validate.py FILE...    # validate manifests, schema chosen by
                                                 # file name (<schema>.*.json)

Needs the `jsonschema` module (python3-jsonschema; 4.10 or later). Works with
the older RefResolver API and the newer `referencing` registry.
"""

import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCHEMA_DIR = os.path.join(ROOT, "schemas")
EXAMPLE_DIR = os.path.join(SCHEMA_DIR, "examples")
MANIFESTS = ("arcpkg", "pack", "model", "model-roles")   # common.schema.json holds shared $defs

# invalid example -> a string that must appear in one of its errors
# (the failing value's JSON path, or the consistency check's message)
EXPECTED = {
    "arcpkg.invalid-install-into-system.json": "$.install_root",
    "arcpkg.invalid-model-without-model.json": "'model' is a required property",
    "arcpkg.invalid-path-escapes.json": "$.files[0].path",
    "arcpkg.invalid-version-mismatch.json": "model.version",
    "model-roles.invalid-unknown-role.json": "writer",
    "model.invalid-embed-without-embedder.json": "'embedder' is a required property",
    "model.invalid-name-not-lowercase-id.json": "$.name",
    "pack.invalid-no-fts.json": "$.provides",
    "pack.invalid-version-not-semver.json": "$.version",
}

try:
    import jsonschema
    from jsonschema import Draft202012Validator
except ImportError:
    sys.exit("validate.py: needs the jsonschema module (apt install python3-jsonschema)")


def load(path):
    with open(path) as f:
        return json.load(f)


def load_schemas():
    """{name: schema} for every schemas/*.schema.json."""
    out = {}
    for path in sorted(glob.glob(os.path.join(SCHEMA_DIR, "*.schema.json"))):
        out[os.path.basename(path)[:-len(".schema.json")]] = load(path)
    return out


def make_validator(schemas, name):
    schema = schemas[name]
    by_id = {s["$id"]: s for s in schemas.values()}
    try:
        from referencing import Registry, Resource          # jsonschema >= 4.18
        registry = Registry().with_resources(
            (uri, Resource.from_contents(s)) for uri, s in by_id.items())
        return Draft202012Validator(schema, registry=registry)
    except ImportError:
        resolver = jsonschema.RefResolver.from_schema(schema, store=by_id)
        return Draft202012Validator(schema, resolver=resolver)


def json_path(error):
    path = "$"
    for p in error.absolute_path:
        path += f"[{p}]" if isinstance(p, int) else f".{p}"
    return path


def consistency(name, doc):
    """Rules a schema cannot express. Returns a list of messages."""
    probs = []
    if name == "arcpkg":
        for inner in ("model", "pack"):
            obj = doc.get(inner)
            if isinstance(obj, dict):
                for key in ("name", "version"):
                    if obj.get(key) != doc.get(key):
                        probs.append(f"$.{inner}.{key} ({obj.get(key)!r}) must equal $.{key} "
                                     f"({doc.get(key)!r}): {inner}.{key} mismatch")
    return probs


def problems(validators, name, doc):
    errs = [f"{json_path(e)}: {e.message}" for e in validators[name].iter_errors(doc)]
    return errs + consistency(name, doc)


def schema_of(path):
    base = os.path.basename(path)
    for name in sorted(MANIFESTS, key=len, reverse=True):
        if base.startswith(name + "."):
            return name
    return None


def main(argv):
    schemas = load_schemas()
    failed = 0
    for name, schema in schemas.items():
        try:
            Draft202012Validator.check_schema(schema)
        except jsonschema.SchemaError as e:
            print(f"FAIL schema {name}: {e.message}")
            failed += 1
    missing = [m for m in MANIFESTS if m not in schemas]
    if missing:
        print(f"FAIL missing schemas: {missing}")
        return 1
    validators = {m: make_validator(schemas, m) for m in MANIFESTS}

    if argv:                                   # validate the given manifests
        for path in argv:
            name = schema_of(path)
            if name is None:
                print(f"FAIL {path}: name it <schema>.<anything>.json, schema one of {MANIFESTS}")
                failed += 1
                continue
            probs = problems(validators, name, load(path))
            for p in probs:
                print(f"FAIL {path}: {p}")
            failed += bool(probs)
            if not probs:
                print(f"ok   {path} ({name})")
        return 1 if failed else 0

    seen = {m: {"valid": 0, "invalid": 0} for m in MANIFESTS}
    for path in sorted(glob.glob(os.path.join(EXAMPLE_DIR, "*.json"))):
        base = os.path.basename(path)
        name = schema_of(path)
        kind = base[len(name) + 1:].split("-")[0].split(".")[0] if name else None
        if kind not in ("valid", "invalid"):
            print(f"FAIL {base}: name examples <schema>.valid[-x].json or <schema>.invalid-<reason>.json")
            failed += 1
            continue
        seen[name][kind] += 1
        probs = problems(validators, name, load(path))
        if kind == "valid":
            for p in probs:
                print(f"FAIL {base} should pass: {p}")
            failed += bool(probs)
        else:
            want = EXPECTED.get(base)
            if want is None:
                print(f"FAIL {base}: add its expected error to EXPECTED in {os.path.basename(__file__)}")
                failed += 1
            elif not probs:
                print(f"FAIL {base} should fail ({want}) but passes")
                failed += 1
            elif not any(want in p for p in probs):
                print(f"FAIL {base} fails for another reason; expected '{want}', got: {probs}")
                failed += 1
    for name, counts in seen.items():
        if counts["valid"] == 0 or counts["invalid"] == 0:
            print(f"FAIL {name}: needs at least one valid and one invalid example ({counts})")
            failed += 1
    for base in sorted(set(EXPECTED) - {os.path.basename(p) for p in glob.glob(os.path.join(EXAMPLE_DIR, "*.json"))}):
        print(f"FAIL EXPECTED names a missing example: {base}")
        failed += 1
    total = sum(c["valid"] + c["invalid"] for c in seen.values())
    print(f"validate.py: {len(schemas)} schemas, {total} examples, {failed} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
