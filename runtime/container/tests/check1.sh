findmnt -n -o TARGET,SOURCE,FSTYPE,OPTIONS -T /home; findmnt -n -o TARGET,SOURCE -T /var/lib/docker
ls -la /dev/fuse /dev/net/tun
exec 3<>/dev/fuse && echo "root: /dev/fuse opens"; exec 3>&-
exec 4<>/dev/net/tun && echo "root: /dev/net/tun opens"; exec 4>&-
su -s /bin/bash nobody -c 'exec 3<>/dev/fuse && echo "nobody: /dev/fuse opens"; exec 4<>/dev/net/tun && echo "nobody: /dev/net/tun opens"'
find /home -printf '%p %u %g %m %s %n %T@ %y\n' | sort > /root/mig-after.txt
(getfattr -R -d -m - --absolute-names /home/ubuntu/mig 2>/dev/null; getfacl -R -p /home/ubuntu/mig 2>/dev/null) > /root/mig-after-attrs.txt
echo "== manifest diff (before vs after):"; diff /root/mig-before.txt /root/mig-after.txt && echo identical
echo "== attrs diff:"; diff /root/mig-before-attrs.txt /root/mig-after-attrs.txt
stat -c '%i %h %n' /home/ubuntu/mig/h1 /home/ubuntu/mig/h2; du -k /home/ubuntu/mig/sparse; tail -c4 /home/ubuntu/mig/sparse; echo
/home/ubuntu/mig/suid -u; su -s /bin/bash nobody -c '/home/ubuntu/mig/suid -u; /home/ubuntu/mig/sgid -g'
systemctl is-system-running; systemctl --failed --no-legend | head
