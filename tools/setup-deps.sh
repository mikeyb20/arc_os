#!/usr/bin/env bash
# Kept for old instructions: fetches the pinned Limine. Use tools/dev-setup.sh for everything.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/dev-setup.sh" limine
