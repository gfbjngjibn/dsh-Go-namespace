# runfail.sh [DSHNS]: on the device as root. A `dshns run` that fails must leave no cgroup of its own.
# Uses a container of its own (dshtest, an empty rootfs under /data/local/tmp), not the user's.
# DSHNS: the launcher to test; the installed one without it. One pushed with adb must be copied
# first (`cp`): adb's writer can still hold the file open, and running it then fails with "Text file busy".
T=/data/local/tmp/dshtest
L=${1:-$(ls /data/app/*/io.github.ccch1mneyyy.dsh_app*/lib/arm64/libdshns.so | head -1)}
rm -rf $T; mkdir -p $T/rootfs $T/state
dd if=/dev/zero of=$T/storage.img bs=1M count=4 2>/dev/null
state() { echo "  dsh-go: [$(ls /sys/fs/cgroup/dsh-go 2>/dev/null | grep -E '^(dsh|dshtest|monitor)' | tr '\n' ' ')] root: [$(cat /sys/fs/cgroup/cgroup.subtree_control)] mark: [$(ls $T/state/pids.root 2>/dev/null | wc -l)] user's container: pids.max [$(cat /sys/fs/cgroup/dsh-go/dsh/pids.max 2>/dev/null)] init [$(cat /data/user/0/io.github.ccch1mneyyy.dsh_app/files/system/state/init.pid 2>/dev/null)]"; }
run() { $L run --name dshtest --state $T/state --rootfs $T/rootfs --uid 10346 --gid 10346 --range 2100000000:1048575 "$@" -- /bin/true 2>&1 | grep -v '^e2fsck\|^ext2fs\|^$' | tail -2; }
echo "== before"; state
echo "== run fails in storage_attach (the storage is no ext4)"; run --storage $T/storage.img --storage-size 34359738368; state
echo "== run fails in the container's PID 1 (empty rootfs, no storage)"; run; state
rmdir /sys/fs/cgroup/dsh-go/dshtest 2>/dev/null && echo "LEFTOVER: dsh-go/dshtest was still there (removed now)"
rm -rf $T
echo "== after"; state
