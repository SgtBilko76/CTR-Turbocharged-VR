# CTR: Turbocharged VR (Meta Quest)

Crash Team Racing: Turbocharged running natively on Meta Quest 3 / 3S / Pro / 2,
built from this repository's `quest` branch (the upstream 64-bit branch plus a
GLES renderer path and an OpenXR layer).

Stage 1: the game plays on a large virtual screen in front of you.

## What you need

- Your own **NTSC-U (US) Crash Team Racing** disc image as a raw `.bin`
  (SCUS-94426). Game data is not included.
- The APK (`android/app/build/outputs/apk/debug/app-debug.apk` after a build).

## Install

1. Install the APK (SideQuest, or `adb install -r app-debug.apk`).
2. Copy the disc image to the headset as `/sdcard/CTR/assets/ctr-u.bin`
   (in SideQuest: the folder icon, then `CTR/assets`).
3. Start **CTR Turbocharged VR** from Library > Unknown Sources. On the first
   start the Quest asks for file access: allow it, then start the app again.

Saves, `config.ini` and the log file are kept in `/sdcard/CTR`.

## Controls

The Touch controllers act as a PlayStation pad; the game's own controller
options can remap everything.

| Touch | PlayStation | In a race (default) |
|---|---|---|
| A | Cross | accelerate |
| B | Circle | weapon |
| X | Square | brake / reverse |
| Y | Triangle | |
| Right trigger | R2 | |
| Left trigger | L2 | |
| Right grip | R1 | jump / power-slide |
| Left grip | L1 | jump / power-slide |
| Left stick | analog stick | steer |
| Right stick | D-pad | menus |
| Left stick click | Select | |
| Right stick click | R3 | |
| Menu button (left) | Start | pause |

Hold **both stick clicks for one second** to put the screen back in front of you.

## Settings (optional)

`/sdcard/CTR/vr.cfg`, one `key = value` per line:

| Key | Default | Meaning |
|---|---|---|
| `screen_width` | 3.0 | screen width in metres |
| `screen_distance` | 2.4 | distance from your head in metres |
| `screen_height` | 0.0 | metres above eye level |
| `refresh_rate` | 120 | display refresh rate (72, 80, 90, 120) |
| `screen_pixels_w` / `screen_pixels_h` | 1920 / 1080 | game resolution |

## Building

```
cd android
JAVA_HOME=<JDK 17> ./gradlew assembleDebug
```

Needs the Android SDK with NDK 25.1 and CMake 3.22.1. `tools/quest/quest_run.sh`
builds, installs, pushes the disc image, launches and records the log;
`tools/quest/quest_cmd.sh` sends pad presses for headless tests.

Desktop checks of the Quest code paths:
`cmake -B build-gles -DCTR_NATIVE_64BIT=ON -DCTR_NATIVE_GLES=ON -DCTR_QUEST_DESKTOP_TEST=ON`
renders through the same GLES path and window hooks as the headset.
