#!/bin/sh
set -eu
# Explicit sibling paths supplied by the caller; no audio or cloud connections.
[ "$#" = 2 ] || { echo 'usage: sh tools/test-application-adapters.sh /path/to/satellite /path/to/voice-client' >&2; exit 2; }
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
satellite=$(CDPATH= cd -- "$1" && pwd)
voice=$(CDPATH= cd -- "$2" && pwd)
stage=$(mktemp -d /tmp/lotus-adapters.XXXXXX)
daemon_pid=''
cleanup() {
    [ -z "$daemon_pid" ] || { kill -TERM "$daemon_pid" 2>/dev/null || true; wait "$daemon_pid" 2>/dev/null || true; }
    # Only our known files, no recursive removal or unresolved broad target.
    for file in service.log control.sock satellite-test voice-test; do rm -f "$stage/$file"; done
    rmdir "$stage"
}
trap cleanup EXIT INT TERM
g++ -std=c++14 -Wall -Wextra -Werror -DLOTUS_HARDWARE_SERVICE=1 -I"$satellite/include" \
    -isystem "$repo/src" -isystem "$repo/third_party/picojson" \
    "$satellite/tests/test_service_backend.cpp" -pthread -o "$stage/satellite-test"
g++ -std=c++11 -Wall -Wextra -Werror -DLOTUS_HARDWARE_SERVICE=1 \
    -I"$voice/common" -I"$voice/control_center" -isystem "$repo/src" -isystem "$repo/third_party/picojson" \
    "$voice/tests/test_service_backend.cpp" "$voice/control_center/hardware_mcp.cpp" \
    "$voice/common/playback_volume.cpp" -pthread -o "$stage/voice-test"
for test in satellite-test voice-test; do
    "$repo/build/lotus-hardwared" --mock --socket "$stage/control.sock" 2>"$stage/service.log" &
    daemon_pid=$!
    retries=0
    while [ ! -S "$stage/control.sock" ]; do
        retries=$((retries+1)); [ "$retries" -lt 5 ] || exit 1; sleep 1
    done
    "$stage/$test" "$stage/control.sock"
    kill -TERM "$daemon_pid"; wait "$daemon_pid"; daemon_pid=''
done
