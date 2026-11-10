#!/bin/bash
# Restarts the engine from the engine page (stop + start), waits until it runs.
D=$(dirname "$0"); ADB=~/android-sdk/platform-tools/adb
$D/stop-engine.sh || exit 1
for i in 1 2 3 4 5 6; do $ADB shell input swipe 400 600 400 2200 150; done
$D/ui.py tapx '启动' || exit 1
for i in $(seq 1 60); do $D/ui.py has '运行中 ·' >/dev/null 2>&1 && { echo "engine running"; exit 0; }; sleep 2; done; exit 1
