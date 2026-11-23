#!/usr/bin/env bash
# Optional maintainer check: a fresh ZIP, an owned game and a private desktop.
# Drives the real wizard, waits for actual work, and closes each title before
# the next launch. No user prefix, player state or desktop shortcut is reused.
set -euo pipefail
if [[ ${1:-} != --inner ]]; then
    : "${1:?usage: installer_e2e.sh RELEASE.zip GAME-DIR EMPTY-TEST-DIR}"
    archive=$(realpath "$1"); game=$(realpath "$2"); testdir=$(realpath -m "$3")
    mkdir "$testdir"
    mkdir "$testdir/Extracted ZIP" "$testdir/proof"
    unzip -q "$archive" -d "$testdir/Extracted ZIP"
    export XDG_DATA_HOME="$testdir/user data" XDG_CACHE_HOME="$testdir/cache" XDG_BIN_HOME="$testdir/bin"
    if [[ -n ${FB_TEST_PREVIOUS:-} ]];then
        # Reuse only compiler outputs from an interrupted install. No
        # registry, save or native Vulkan cache is copied into the test.
        previous=$(realpath "$FB_TEST_PREVIOUS")
        mkdir -p "$XDG_CACHE_HOME/frostybird"
        for kind in compiler-v1 hlsl-v1;do
            if [[ -d $previous/cache/frostybird/$kind ]];then
                cp -al "$previous/cache/frostybird/$kind" "$XDG_CACHE_HOME/frostybird/"
            fi
        done
        shopt -s nullglob
        for old in "$previous/user data/frostybird/releases"/*;do
            id=${old##*/}
            old_source="$previous/cache/frostybird/$id/hlsl"
            if [[ ! -d $old_source || ! -r $old/lib/libdxil.so ]];then continue;fi
            dest="$XDG_DATA_HOME/frostybird/releases/$id/lib"
            mkdir -p "$dest" "$XDG_CACHE_HOME/frostybird/$id"
            cp -l "$old"/lib/{libdxcompiler.so,libdxil.so} "$dest/"
            cp -al "$old_source" "$XDG_CACHE_HOME/frostybird/$id/"
        done
    fi
    opts=();if [[ -n ${FB_TEST_GPU:-} ]];then opts+=(--prefer-vk-device "$FB_TEST_GPU");fi
    sink=frostybird_test_$$
    module=$(pactl load-module module-null-sink "sink_name=$sink" rate=48000 channels=2)
    trap 'pactl unload-module "$module"' EXIT
    env -u DISPLAY -u WAYLAND_DISPLAY PULSE_SINK="$sink" gamescope --backend headless "${opts[@]}" \
        -w 1280 -h 720 -W 1280 -H 720 -- bash "$(realpath "$0")" --inner "$game" "$testdir" \
        >"$testdir/proof/compositor.log" 2>&1
    cat "$testdir/proof/result.txt"
    exit
fi
game=$2; base=$3; app="$base/Extracted ZIP/FrostyBird/frostybird"; proof="$base/proof"
export SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=dummy
gui=; child=; worker=
stop_wine(){
    local server prefix release
    shopt -s nullglob
    for prefix in "$XDG_DATA_HOME"/frostybird/instances/*/*/prefix; do
        release=$(basename "$(dirname "$(dirname "$prefix")")")
        server="$XDG_DATA_HOME/frostybird/releases/$release/wine/bin/wineserver"
        if [[ -x $server ]];then
            WINEPREFIX="$prefix" "$server" -k >/dev/null 2>&1 || true
            WINEPREFIX="$prefix" timeout 20 "$server" -w >/dev/null 2>&1 || true
        fi
    done
}
cleanup(){ if [[ -n $worker ]];then kill -- "-$worker" 2>/dev/null || true;fi
    stop_wine;for p in "$gui" "$child";do if [[ -n $p ]];then kill "$p" 2>/dev/null || true;fi;done; }
trap cleanup EXIT
title_window(){
    timeout 210 xdotool search --sync --onlyvisible --class '^frosty-xsx-shipping[.]exe$' | head -1
}
first_present(){
    local marker='first GPU present to UnrealWindowGDK'
    timeout 150 bash -c 'until grep -Fq "$1" "$2" 2>/dev/null; do sleep .25; done' \
        _ "$marker" "$playlog"
    grep -Fm1 "$marker" "$playlog"
}
snapshot(){ gamescopectl screenshot "$proof/$1.png" >/dev/null 2>&1 || true; }
"$app" --source "$game" >"$proof/wizard.log" 2>&1 & gui=$!
w=$(timeout 35 xdotool search --sync --onlyvisible --name '^Frosty Bird.*The Matrix Awakens$' | head -1)
xdotool windowfocus "$w"
sleep 1.3
snapshot welcome
pactl list sink-inputs >"$proof/setup-audio.txt"
press(){
    xdotool keydown --window "$w" "$1"
    sleep .06
    xdotool keyup --window "$w" "$1"
    sleep .34
}
press Right
press Return
snapshot review
for key in Right Return space Right; do
    press "$key"
    worker=$(pgrep -P "$gui" -f -- '--worker' | head -1 || true)
    if [[ -n $worker ]]; then break;fi
done
if [[ -z $worker ]];then snapshot start-error;echo 'Wizard did not start the installer worker' >&2;exit 1;fi
snapshot syncing
printf '%s\n' "$worker" >"$proof/worker.pid"
timeout 2700 pidwait -F "$proof/worker.pid" || { test ! -e "/proc/$worker" || exit 1; }
worker=
state="$XDG_DATA_HOME/frostybird/installed.json";test -s "$state"
shopt -s nullglob
reports=( "$XDG_DATA_HOME"/frostybird/instances/*/*/logs/pipeline-result.txt )
if (( ${#reports[@]} ));then
    cat "${reports[@]}" >"$proof/pipelines.txt"
    grep -Eq '^[1-9][0-9]* native pipelines' "$proof/pipelines.txt"
fi
grep -F 'frostybird/pso-cache-committed' "$XDG_DATA_HOME"/frostybird/instances/*/*/logs/pipelines*.log >/dev/null
playlogs=( "$XDG_DATA_HOME"/frostybird/instances/*/*/logs )
test "${#playlogs[@]}" -eq 1
playlog="${playlogs[0]}/play.log"
w=$(title_window);xprop -id "$w" WM_CLASS _NET_WM_PID >"$proof/first-launch.txt"
first_present >>"$proof/first-launch.txt"
stop_wine; wait "$gui" || true; gui=
stamp=$(stat -c '%y' "$state")
mv "$base/Extracted ZIP" "$base/Removed download"
"$XDG_BIN_HOME/frostybird" >"$proof/second-launch.log" 2>&1 & child=$!
w=$(title_window);xprop -id "$w" WM_CLASS _NET_WM_PID >"$proof/second-launch.txt"
first_present >>"$proof/second-launch.txt"
test "$stamp" = "$(stat -c '%y' "$state")"
stop_wine; wait "$child" || true;child=
printf 'PASS: clean GUI install, automatic title launch, installed-only relaunch.\n' >"$proof/result.txt"
