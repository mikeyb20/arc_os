# ports/ — outside code, built from the archive

One folder per outside component: `PORT.md` (version, SHA-256 source in `third_party/SOURCES.lock`, licence,
options and why, known gaps), `build.sh <prefix>` (offline, installs headers, static libraries and tools into the
prefix) and `patches/` (numbered, each with its reason). A port's folder name is its `SOURCES.lock` entry: every
`build.sh` sources `tools/lane/port-env.sh` (the musl compiler and flags, `port_begin` to copy the archived source
to `build-lane/work/<name>/` and apply the patches, `port_check_static`), so upstream stays untouched and the build
needs no network. How to add one: the `port-package` skill
(`.claude/skills/port-package/`). Lane conventions: [`docs/plans/lane-l.md`](../docs/plans/lane-l.md).

`tools/lane/build.sh` fetches every port's source, then runs every `build.sh` here into `build-lane/ports/<name>/`
(imported in `stack/` as `port::<library>` targets), except `musl-cross-make`, which
builds the toolchain itself (published by `.github/workflows/toolchain.yml`, installed by `tools/dev-setup.sh musl`).
