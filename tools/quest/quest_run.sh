#!/bin/bash
# Build, install and launch CTR Turbocharged VR on a Quest over adb, then
# record the app log and take a headset screenshot.
#
# usage: tools/quest/quest_run.sh [--no-build] [--no-install] [SECONDS]
#   SECONDS: how long to record the log before the screenshot (default 40)
#
# Pushes the disc image from ~/workspace/ctr-data/ctr-u.bin to
# /sdcard/CTR/assets/ctr-u.bin if the headset doesn't have it yet.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ADB=${ADB:-$HOME/workspace/android-sdk/platform-tools/adb}
PKG=com.bilko.ctrvr
APK=$ROOT/android/app/build/outputs/apk/debug/app-debug.apk
DISC=${DISC:-$HOME/workspace/ctr-data/ctr-u.bin}
OUT=${OUT:-$HOME/workspace/ctr-data/quest}
BUILD=1
INSTALL=1
SECS=40
for a in "$@"; do
  case $a in
    --no-build) BUILD=0;;
    --no-install) INSTALL=0; BUILD=0;;
    *) SECS=$a;;
  esac
done
mkdir -p "$OUT"
a() { timeout 60 "$ADB" "$@"; }

if [ $BUILD = 1 ]; then
  (cd "$ROOT/android" && JAVA_HOME=$HOME/.local/jdk-17.0.20.1+1 ./gradlew assembleDebug --no-daemon -q) > "$OUT/gradle.log" 2>&1 \
    || { echo "BUILD FAILED"; tail -30 "$OUT/gradle.log"; exit 1; }
fi

a shell echo ok >/dev/null || { echo "no headset on adb"; exit 1; }
if a shell dumpsys activity activities | grep -q "topResumedActivity.*$PKG"; then
  echo "NOTE: $PKG is in the foreground right now (someone may be playing)."
fi

if [ $INSTALL = 1 ]; then
  timeout 300 "$ADB" install -r "$APK" || { echo "INSTALL FAILED"; exit 1; }
fi
a shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
a shell mkdir -p /sdcard/CTR/assets
remote_size=$(a shell stat -c %s /sdcard/CTR/assets/ctr-u.bin 2>/dev/null | tr -d '\r')
local_size=$(stat -c %s "$DISC")
if [ "$remote_size" != "$local_size" ]; then
  echo "pushing disc image ($local_size bytes)..."
  timeout 600 "$ADB" push "$DISC" /sdcard/CTR/assets/ctr-u.bin || { echo "PUSH FAILED"; exit 1; }
fi

# Keep the headset awake and out of the Guardian dialog for unattended runs.
a shell setprop debug.oculus.guardian_pause 1
a shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null
a shell input keyevent KEYCODE_WAKEUP
a shell am force-stop $PKG
a logcat -c
a shell am start -n $PKG/.CTRActivity >/dev/null
timeout "$SECS" "$ADB" logcat -v time CTRQuest:V CTR:V CTRVR:V SDL:V SDL/APP:V VrApi:I DEBUG:V '*:F' > "$OUT/run.log" 2>&1
pid=$(a shell pidof $PKG | tr -d '\r')
echo "pid: ${pid:-not running}"
a shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT >/dev/null
sleep 3
shot=$(a shell ls -t /sdcard/Oculus/Screenshots | head -1 | tr -d '\r')
[ -n "$shot" ] && a pull "/sdcard/Oculus/Screenshots/$shot" "$OUT/$shot" >/dev/null && echo "screenshot: $OUT/$shot"
grep -E "CTRQuest|FATAL|#[0-9][0-9] " "$OUT/run.log" | head -40
[ -n "$pid" ] && grep "VrApi" "$OUT/run.log" | grep "$pid" | tail -2
echo "log: $OUT/run.log"
