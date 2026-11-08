#!/bin/sh
# Run one arc_os-bound program for a lane test (lane-l.md, M2-01).
#
#   tools/lane/run.sh <log-name> <command> [args...]
#
# Normally just runs the command. With ARC_LANE_LOGS set (lane.yml does), runs it
# under `strace -f` and writes $ARC_LANE_LOGS/<log-name>.strace for the syscall
# check (tools/syscall-inventory.py --check). The exit status is the command's.
# Test drivers (shell, Python, curl) never go through here: they don't reach arc_os.
set -eu
name=$1
shift
if [ -z "${ARC_LANE_LOGS:-}" ]; then
    exec "$@"
fi
mkdir -p "$ARC_LANE_LOGS"
exec strace -f -qq -s 200 -o "$ARC_LANE_LOGS/$name.strace" -- "$@"
