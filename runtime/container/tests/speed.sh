t() { python3 -c 'import time;print(time.monotonic())'; }
el() { python3 -c "print(round($2-$1,1))"; }
dd if=/dev/urandom of=/var/tmp/big bs=1M count=2048 status=none; sync
s=$(t); cp -a /var/tmp/big /home/big; sync; e=$(t); echo "2 GiB file (page cache warm, source just written): $(el $s $e) s"
rm -f /home/big /var/tmp/big; sync
mkdir -p /var/tmp/many; for i in $(seq 1 20000); do echo $i > /var/tmp/many/f$i; done; sync
s=$(t); cp -a /var/tmp/many /home/many; sync; e=$(t); echo "20000 small files: $(el $s $e) s"
rm -rf /home/many /var/tmp/many; sync
