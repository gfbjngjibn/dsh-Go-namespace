set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq acl attr sshfs squashfuse squashfs-tools openssh-server libfuse2t64 >/dev/null 2>&1 || apt-get install -y acl attr sshfs squashfuse squashfs-tools openssh-server libfuse2t64 | tail -5
dpkg -l acl attr sshfs squashfuse squashfs-tools openssh-server libfuse2t64 | grep ^ii | awk '{print $2,$3}'
M=/home/ubuntu/mig
rm -rf $M; mkdir -p $M/sub; cd $M
cp /usr/bin/id suid; chown ubuntu:ubuntu suid; chmod 4755 suid
cp /usr/bin/id sgid; chown root:ubuntu sgid; chmod 2755 sgid
echo x > x; setfattr -n user.dsh -v hello x
echo acl > acl; setfacl -m u:nobody:rw,g:adm:r acl
setfacl -d -m u:ubuntu:rwx sub
echo h > h1; ln h1 h2
ln -s ../.bashrc link
mkfifo fifo
truncate -s 100M sparse; printf 'tail' >> sparse
echo p > private; chmod 600 private; chmod 700 sub
echo o > other; chown 1234:5678 other
echo old > old; touch -d '2001-01-01 01:02:03' old
echo s > 'with space'
chown -R ubuntu:ubuntu sub h1 x acl link fifo sparse private old 'with space' 2>/dev/null; chown -h ubuntu:ubuntu link
cd /
find /home -printf '%p %u %g %m %s %n %T@ %y\n' | sort > /root/mig-before.txt
(getfattr -R -d -m - --absolute-names /home/ubuntu/mig 2>/dev/null; getfacl -R -p /home/ubuntu/mig 2>/dev/null) > /root/mig-before-attrs.txt
wc -l /root/mig-before.txt; du -sh --apparent-size $M/sparse; du -sh $M/sparse; cat /root/mig-before-attrs.txt | head -40
