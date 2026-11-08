#!/usr/bin/env bash
# Fetch outside components from Michael's archive (M0-03), never from upstream.
#
#   tools/fetch-archived.sh llama.cpp sqlite tiny-chat-model
#
# Each name is an entry in third_party/SOURCES.lock. The file comes from its
# archive_url (a release asset on GitHub), is checked against the pinned SHA-256
# and is unpacked into third_party/src/<name>/ (gitignored). Downloads are cached
# in third_party/cache/, so a second run is offline. Exit status 1 if any failed.
set -euo pipefail
if [ "$#" -eq 0 ]; then
    echo "usage: $0 <component>..." >&2
    echo "components:" >&2
    python3 -c 'import json,sys; [print("  " + c["name"]) for c in json.load(open(sys.argv[1]))["components"]]' \
        "$(dirname "$0")/../third_party/SOURCES.lock" >&2
    exit 2
fi
exec python3 "$(dirname "$0")/archive.py" fetch "$@"
