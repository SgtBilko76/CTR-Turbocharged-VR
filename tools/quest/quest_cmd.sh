#!/bin/bash
# Send virtual pad presses to the running Quest build and take a screenshot.
# usage: tools/quest/quest_cmd.sh 'a 200\nwait 3000' [settle_seconds] [name]
# Buttons: a b x y l1 r1 l2 r2 start select up down left right steerleft steerright wait
set -u
ADB=${ADB:-$HOME/workspace/android-sdk/platform-tools/adb}
OUT=${OUT:-$HOME/workspace/ctr-data/quest}
CMDS=$1
SETTLE=${2:-3}
NAME=${3:-cmd}
mkdir -p "$OUT"
printf "$CMDS\n" > "$OUT/vrcmd"
timeout 20 "$ADB" push "$OUT/vrcmd" /sdcard/CTR/vrcmd >/dev/null || exit 1
# wait until the app has consumed the file, then let the steps run
for i in $(seq 1 20); do
  timeout 5 "$ADB" shell ls /sdcard/CTR/vrcmd >/dev/null 2>&1 || break
  sleep 0.5
done
sleep "$SETTLE"
timeout 10 "$ADB" shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT >/dev/null
sleep 3
shot=$(timeout 10 "$ADB" shell ls -t /sdcard/Oculus/Screenshots | head -1 | tr -d '\r')
timeout 20 "$ADB" pull "/sdcard/Oculus/Screenshots/$shot" "$OUT/$NAME.jpg" >/dev/null && echo "$OUT/$NAME.jpg ($shot)"
