#!/bin/bash
# Headless desktop test of ctr_native under Xvfb (software GL).
# usage: BIN=/path/ctr_native TAG=name run_headless.sh STEP...
#   STEP = KEY | sleep:N | shot:NAME | hold:KEY:SECS
# Scratch dir S (default ~/workspace/ctr-data/run) gets the binary + assets symlink.
set -u
T=$(cd "$(dirname "$0")" && pwd)
S=${S:-$HOME/workspace/ctr-data/run}
TAG=${TAG:-run}
mkdir -p "$S" "$S/shots"
cp -f "$BIN" "$S/ctr_native"
[ -e "$S/assets" ] || { mkdir -p "$S/assets"; cp -r "$(dirname "$BIN")/assets/." "$S/assets/"; ln -sf "$HOME/workspace/ctr-data/ctr-u.bin" "$S/assets/ctr-u.bin"; }
export DISPLAY=:77
Xvfb :77 -screen 0 1920x1200x24 -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
export SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
cd "$S"
bwrap --die-with-parent --dev-bind / / --tmpfs /dev/input --tmpfs /run/udev \
  ./ctr_native ${GAMEARGS:-} > "$S/stdout_$TAG.txt" 2>&1 &
P=$!
for st in "$@"; do
  kill -0 $P 2>/dev/null || { echo "DIED before step $st"; break; }
  case $st in
    sleep:*) sleep "${st#sleep:}";;
    shot:*) ffmpeg -loglevel error -y -f x11grab -video_size 1920x1200 -i :77 -frames:v 1 "$S/shots/${TAG}_${st#shot:}.png";;
    click:*) xy=${st#click:}; "$T/xkey" click "${xy%,*}" "${xy#*,}";;
    move:*) xy=${st#move:}; "$T/xkey" move "${xy%,*}" "${xy#*,}";;
    keydown:*) "$T/xkey" down "${st#keydown:}";;
    keyup:*) "$T/xkey" up "${st#keyup:}";;
    hold:*) k=${st#hold:}; "$T/xkey" tap "${k%%:*}" "${k##*:}";;
    *) "$T/xkey" tap "$st"; sleep 0.4;;
  esac
done
kill -0 $P 2>/dev/null && echo ALIVE || { wait $P; echo "exit=$?"; }
kill -TERM $P 2>/dev/null; sleep 2; kill -9 $P 2>/dev/null
kill $XP 2>/dev/null
true
