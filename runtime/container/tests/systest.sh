A=/data/user/0/io.github.ccch1mneyyy.dsh_app/files; T=$A/systest; R=$T/rootfs
L=$(ls /data/app/*/io.github.ccch1mneyyy.dsh_app*/lib/arm64/libdshns.so | head -1)
cat > $R/tmp/t.sh <<'IN'
echo "  guest: /home on $(findmnt -n -o SOURCE,FSTYPE -T /home | tr -s ' ')"
echo "  guest: /home/ubuntu: $(ls -A /home/ubuntu 2>&1 | tr '\n' ' ')"
echo "  guest: storage: $(ls -A /var/lib/dsh-go/storage | tr '\n' ' ') marker=$(test -e /var/lib/dsh-go/home-on-storage && echo yes || echo no)"
s=$(stat -c '%U:%G %a' /home/ubuntu/secret 2>/dev/null) && echo "  guest: secret $s"
exec 3<>/dev/fuse && exec 4<>/dev/net/tun && echo "  guest: /dev/fuse and /dev/net/tun open"
IN
chmod 644 $R/tmp/t.sh
run() { su 10346 -c "su -W --ksu-no-new-privs -c '$L run --name dshtest --state $T/state --rootfs $R --uid 10346 --gid 10346 --range 2100000000:1048575 --storage $T/storage.img --storage-size 34359738368 -- /bin/bash /tmp/t.sh'" 2>&1 | grep -vE 'e2fsck|dsh-storage:|storage .* on /dev|created \(' ; echo "  exit=${PIPESTATUS[0]}"; }
host() { echo "  host: rootfs/home: [$(ls -A $R/home 2>&1 | tr '\n' ' ')] cgroup: [$(ls /sys/fs/cgroup/dsh-go | grep -E '^(dsh|dshtest|monitor)' | tr '\n' ' ')] root pids: [$(cat /sys/fs/cgroup/cgroup.subtree_control)]"; }
echo "== T1 fresh rootfs, no storage"; run; host
echo "== T2 again: no second move"; run; host
echo "== T3 copy fails: a file container root cannot read (owner unmapped, 0600)"
rm -f $T/storage.img $R/var/lib/dsh-go/home-on-storage; rmdir $R/home 2>/dev/null; mkdir -p $R/home/ubuntu; echo a > $R/home/ubuntu/.bashrc; echo s > $R/home/ubuntu/secret
chown 10346:10346 $R/home; chown -R 2100000999:2100000999 $R/home/ubuntu; chown 0:0 $R/home/ubuntu/secret; chmod 600 $R/home/ubuntu/secret; chmod 750 $R/home/ubuntu
run; host
echo "== T4 fixed (secret readable): retry clears home.partial"
chmod 644 $R/home/ubuntu/secret; run; host
echo "== T5 crash between dropping the original and making the mount point"
rmdir $R/home; run; host
echo "== T6 marker missing, storage has /home"
rm $R/var/lib/dsh-go/home-on-storage; run; host; ls $R/var/lib/dsh-go/home-on-storage
echo "== T7 storage lost /home (new storage, marker present)"
rm -f $T/storage.img; run; host
echo "== user's container untouched"; cat /sys/fs/cgroup/dsh-go/dsh/pids.max; cat $A/system/state/init.pid
