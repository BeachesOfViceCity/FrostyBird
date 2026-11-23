#!/usr/bin/env bash
# Real Vulkan preview on a private display; no game launch or writes to the
# installation. The separate frostybird-cue-check tests the SDL PCM mix.
set -euo pipefail
if [[ ${1:-} != --inner ]]; then
    app=$(realpath "${1:?usage: installer_preview.sh EXTRACTED/frostybird EMPTY-RESULT-DIR}")
    result=$(realpath -m "$2")
    mkdir "$result"
    export XDG_DATA_HOME="$result/data" XDG_CACHE_HOME="$result/cache" XDG_BIN_HOME="$result/bin"
    env -u DISPLAY -u WAYLAND_DISPLAY gamescope --backend headless \
        -w 1280 -h 720 -W 1280 -H 720 -- bash "$(realpath "$0")" --inner "$app" "$result" \
        >"$result/compositor.log" 2>&1
    cat "$result/result.txt"
    exit
fi
app=$2; result=$3
export SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=dummy
gui=
trap 'if [[ -n $gui ]];then kill "$gui" 2>/dev/null || true;fi' EXIT
"$app" --preview --mute >"$result/wizard.log" 2>&1 & gui=$!
w=$(timeout 35 xdotool search --sync --onlyvisible --name '^Frosty Bird.*The Matrix Awakens$' | head -1)
xdotool windowfocus "$w"
sleep 1.2
gamescopectl screenshot "$result/welcome.png"
for key in Right Return Right space Return Right; do
    xdotool keydown --window "$w" "$key"
    sleep .06
    xdotool keyup --window "$w" "$key"
    sleep .34
done
sleep 2
gamescopectl screenshot "$result/shaders.png"
printf '%s\n' "$gui" >"$result/gui.pid"
timeout 13 pidwait -F "$result/gui.pid" || test ! -e "/proc/$gui"
wait "$gui"; gui=
test ! -e "$XDG_DATA_HOME/frostybird"
test ! -e "$XDG_BIN_HOME/frostybird"
printf 'PASS: read-only Vulkan preview, including the shader progress screen.\n' >"$result/result.txt"
