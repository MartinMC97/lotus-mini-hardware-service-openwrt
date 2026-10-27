#!/bin/sh
# Temporary RAM-only control test. The supplied directory contains built binaries.
set -eu
[ "$#" -ge 2 ] && [ "$#" -le 3 ] && [ "$1" = --handoff-confirmed ] || {
    echo 'usage: device-control-smoke.sh --handoff-confirmed /tmp/lotus-hw-control.XXXXXX [--audio-test|--audio-compare]' >&2
    exit 2
}
stage=$2
audio=0
if [ "$#" = 3 ]; then
    case "$3" in --audio-test) audio=1;; --audio-compare) audio=2;; *) exit 2;; esac
    [ -r "$stage/lotus-mini-playback.conf" ] && [ -r "$stage/tone.s16le" ] || exit 2
fi
case "$stage" in /tmp/lotus-hw-control.*) ;; *) exit 2;; esac
[ -d "$stage" ] && [ ! -L "$stage" ] || exit 2
chmod 700 "$stage"
for program in lotus-hardwared lotus-hwctl control-smoke; do
    [ -x "$stage/$program" ] || exit 2
done
if pidof sound_app >/dev/null || pidof control_center >/dev/null || pidof lotus-hardwared >/dev/null; then
    echo 'Unexpected competing service; refusing handoff' >&2; exit 1
fi
restore=0
daemon=
playback=
original_volume=
cleanup() {
    trap - EXIT INT TERM HUP
    if [ -n "$playback" ]; then
        kill -TERM "$playback" 2>/dev/null || true
        wait "$playback" 2>/dev/null || true
    fi
    if [ -n "$daemon" ]; then
        kill -TERM "$daemon" 2>/dev/null || true
        wait "$daemon" 2>/dev/null || true
    fi
    if [ -n "$original_volume" ]; then
        amixer -q -c LotusMini cset "name=HA Volume" "$original_volume"
        echo "Restored exact mixer values: $original_volume"
    fi
    if [ "$restore" = 1 ]; then
        /etc/init.d/ha_voice_satellite start
        echo 'Restored previously running satellite (no test playback requested)'
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
# Explicitly stop the second service as well, even if no main process was found.
/etc/init.d/xiaozhi stop
if pidof satellite >/dev/null; then
    [ "$(cat /var/run/ha-voice-satellite/state)" = idle ] || {
        echo 'Satellite not idle; refusing interruption' >&2; exit 1
    }
    restore=1
    /etc/init.d/ha_voice_satellite stop
    # procd stop and the thermal wrapper/worker teardown are asynchronous.
    attempts=0
    while pidof satellite >/dev/null || pidof microwakeword-worker >/dev/null; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 15 ] || break
        sleep 1
    done
fi
if pidof satellite >/dev/null || pidof microwakeword-worker >/dev/null; then
    echo 'Previous hardware/audio owner still running; refusing control' >&2; exit 1
fi
echo "handoff: old owner stopped, temporary controls; audio_test=$audio"
if [ "$audio" != 0 ]; then
    echo 'Playback PCM before test:'
    cat /proc/asound/card0/pcm0p/sub0/status
    [ "$(cat /proc/asound/card0/pcm0p/sub0/status)" = closed ] || {
        echo 'Playback PCM still owned; refusing test' >&2; exit 1
    }
fi
play_checked() {
    config=$1
    device=$2
    log=$3
    ALSA_CONFIG_PATH="$config" aplay -v -D "$device" -t raw -f S16_LE -r 48000 -c 2 "$stage/tone.s16le" >"$log" 2>&1 &
    playback=$!
    sleep 1
    echo "Expected playback owner_pid: $playback"
    cat /proc/asound/card0/pcm0p/sub0/status
    if pidof satellite >/dev/null || pidof sound_app >/dev/null || pidof control_center >/dev/null; then
        echo 'Assistant restarted during test' >&2; return 1
    fi
    owner=$(sed -n 's/^owner_pid *: *\([0-9]*\)$/\1/p' /proc/asound/card0/pcm0p/sub0/status)
    [ "$owner" = "$playback" ] || { echo 'Unexpected playback owner' >&2; return 1; }
    wait "$playback"
    playback=
}
volume_flag=
if [ "$audio" != 0 ]; then
    original_volume=$(amixer -c LotusMini cget "name=HA Volume" | sed -n 's/.*: values=\([0-9]*,[0-9]*\)$/\1/p')
    left=${original_volume%,*}
    right=${original_volume#*,}
    case "$left$right" in ''|*[!0-9]*) original_volume=; exit 1;; esac
    [ "$left" = "$right" ] && [ "$left" -le 255 ] || { original_volume=; exit 1; }
    volume_flag=--volume-alsa
    echo "Original exact mixer values: $original_volume"
fi
"$stage/lotus-hardwared" --controls --handoff-confirmed --socket "$stage/control.sock" \
    $volume_flag >"$stage/daemon.stdout" 2>"$stage/daemon.log" &
daemon=$!
sleep 1
kill -0 "$daemon"
"$stage/lotus-hwctl" --socket "$stage/control.sock" capabilities.get
if [ "$audio" != 0 ]; then
    "$stage/lotus-hwctl" --socket "$stage/control.sock" state.get
    if [ "$audio" = 2 ]; then
        "$stage/lotus-hwctl" --socket "$stage/control.sock" volume.set '{"percent":60}'
        echo 'A: staged lotus_playback, 60%, 8% PCM peak, warm-up silence'
        play_checked "$stage/lotus-mini-playback.conf" lotus_playback "$stage/playback-A.log"
        sleep 2
        echo 'B: existing ha_playback, same mixer/sample/level'
        play_checked /usr/share/ha-voice-satellite/alsa.conf ha_playback "$stage/playback-B.log"
        amixer -c LotusMini cget "name=HA Volume"
        exit 0
    fi
    "$stage/lotus-hwctl" --socket "$stage/control.sock" volume.set '{"percent":20}'
    ALSA_CONFIG_PATH="$stage/lotus-mini-playback.conf" aplay -q -D lotus_playback -t raw -f S16_LE -r 48000 -c 2 "$stage/tone.s16le"
    "$stage/lotus-hwctl" --socket "$stage/control.sock" volume.adjust '{"delta":10}'
    ALSA_CONFIG_PATH="$stage/lotus-mini-playback.conf" aplay -q -D lotus_playback -t raw -f S16_LE -r 48000 -c 2 "$stage/tone.s16le"
    amixer -c LotusMini cget "name=HA Volume"
    exit 0
fi
if ! "$stage/control-smoke" --socket "$stage/control.sock" >"$stage/smoke.log" 2>&1; then
    cat "$stage/smoke.log"
    exit 1
fi
cat "$stage/smoke.log"
for channel in redled greenled blueled; do
    echo "$channel=$(cat /sys/class/leds/$channel/brightness)"
done
