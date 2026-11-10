#!/bin/sh
# lane.zstd_cli (M2-02): the zstd CLI round-trips a 3 MB file with two worker threads,
# the way package builders compress (M3-04, M5-04). Driver: runs zstd through
# $ARC_LANE_RUN, so each zstd run is traced when ARC_LANE_LOGS is set.
#
#   zstd_cli.sh <zstd>
set -eu
zstd=$1
dir=$(mktemp -d "${TMPDIR:-/tmp}/lane-zstd.XXXXXX")
trap 'rm -rf "$dir"' EXIT
# 3 MB of real machine code: three copies of the zstd binary, cut to 3 000 000 bytes.
cat "$zstd" "$zstd" "$zstd" | head -c 3000000 > "$dir/sample.bin"
"$ARC_LANE_RUN" zstd_cli-compress "$zstd" -q -f -3 -T2 "$dir/sample.bin" -o "$dir/sample.bin.zst"
"$ARC_LANE_RUN" zstd_cli-decompress "$zstd" -q -f -d "$dir/sample.bin.zst" -o "$dir/sample.out"
cmp "$dir/sample.bin" "$dir/sample.out"
echo "zstd_cli: round trip ok ($(wc -c < "$dir/sample.bin.zst") compressed bytes)"
