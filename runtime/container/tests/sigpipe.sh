# sigpipe.sh [DSHNS]: on the device as root. `dshns start` whose caller dies before the monitor
# answers must still leave a monitor with the container. Starts and stops a container of its own
# (dshtest, the image unpacked to files/systest/rootfs as in README.md). The bridge and its rules
# have one name for every container, so the user's must be stopped first.
A=/data/user/0/io.github.ccch1mneyyy.dsh_app/files; T=$A/systest
L=${1:-$(ls /data/app/*/io.github.ccch1mneyyy.dsh_app*/lib/arm64/libdshns.so | head -1)}
[ -d /sys/fs/cgroup/dsh-go/dsh ] && { echo "the user's container runs: stop it first"; exit 1; }
[ -x $T/rootfs/usr/lib/systemd/systemd ] || { echo "no image in $T/rootfs"; exit 1; }
ARGS="--name dshtest --state $T/state --rootfs $T/rootfs --uid 10346 --gid 10346 --range 2100000000:1048575"
C=/sys/fs/cgroup/dsh-go/monitor-dshtest/cgroup.procs
host() { echo "  host: bridge: [$(ip -br link show dsh-br0 2>/dev/null | cut -c1-24)] rules: [$(ip rule | grep -cE '^60[67]0:')] dsh-go: [$(ls /sys/fs/cgroup/dsh-go 2>/dev/null | grep -E '^(dsh|dshtest|monitor)' | tr '\n' ' ')] root: [$(cat /sys/fs/cgroup/cgroup.subtree_control)] mark: [$(ls $T/state/pids.root 2>/dev/null | wc -l)] ip_forward: $(cat /proc/sys/net/ipv4/ip_forward)"; }
echo "== before"; host
mkdir -p $T/state; rm -f $T/state/out
$L start $ARGS > $T/state/out 2>&1 &
P=$!
# The monitor is in its cgroup before it builds anything; it answers after the network and the container's root.
# (A cgroup file's size is always 0: read it.)
i=0; M=; while [ -z "$M" ] && [ $i -lt 1000 ]; do i=$((i + 1)); M=$(head -1 $C 2>/dev/null); done
kill -9 $P && echo "== killed the caller ($P) with the monitor at $M, after $i polls" || echo "== TOO LATE: the caller had exited"
i=0; while [ ! -s $T/state/init.pid ] && [ $i -lt 30 ]; do i=$((i + 1)); sleep 1; done
sleep 4
echo "  caller's answer: [$(grep -c '^running' $T/state/out)] (0: it died before the monitor answered)"
echo "  container init: [$(cat $T/state/init.pid 2>/dev/null)] alive: $([ -d /proc/$(cut -d' ' -f1 $T/state/init.pid 2>/dev/null) ] && echo yes || echo no)"
echo "  MONITOR alive: $([ -n "$M" ] && [ -d /proc/$M ] && echo yes || echo NO)"
tail -3 $T/state/monitor.log | sed 's/^/  log: /'
host
echo "== stop"; $L stop --name dshtest --state $T/state --timeout 30; sleep 1
host
