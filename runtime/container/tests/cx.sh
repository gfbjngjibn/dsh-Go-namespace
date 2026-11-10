#!/bin/bash
# cx.sh SCRIPT [TIMEOUT]: runs SCRIPT (local file) in the system container as its root.
set -e
ADB=~/android-sdk/platform-tools/adb
S=/data/user/0/io.github.ccch1mneyyy.dsh_app/files/system
T=${2:-600}
$ADB exec-in "su -c 'cat > $S/rootfs/tmp/x.sh && chown 10346:10346 $S/rootfs/tmp/x.sh'" < "$1"
$ADB shell su <<EOS
S=$S
L=\$(ls /data/app/*/io.github.ccch1mneyyy.dsh_app*/lib/arm64/libdshns.so | head -1)
F=/data/local/tmp/cx.fifo; rm -f \$F; mkfifo \$F; sleep $T > \$F & H=\$!
su 10346 -c "su -W --ksu-no-new-privs -c '\$L exec --name dsh --state \$S/state -- /bin/bash /tmp/x.sh'" < \$F 2>&1
echo "exit=\$?"
kill \$H; rm -f \$F
EOS
