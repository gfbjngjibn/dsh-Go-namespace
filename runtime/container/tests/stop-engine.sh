#!/bin/bash
D=$(dirname "$0"); ADB=~/android-sdk/platform-tools/adb
$ADB shell am start -a android.intent.action.VIEW -d dshgo://open/engine >/dev/null; sleep 3
for i in 1 2 3 4 5 6; do $ADB shell input swipe 400 600 400 2200 150; done
$D/ui.py has '已停止' >/dev/null 2>&1 && { echo "engine already stopped"; exit 0; }
$D/ui.py tapx '停止' || exit 1; sleep 2
$D/ui.py has '停止引擎？' || exit 1; $D/ui.py tapx '停止' || exit 1
for i in $(seq 1 20); do $D/ui.py has '已停止' >/dev/null 2>&1 && { echo "engine stopped"; exit 0; }; sleep 1; done; exit 1
