python3 - <<'PY'
def blocks(path):
    out, cur = {}, None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("# file: "):
            cur = line[8:]; out.setdefault(cur, set())
        elif line and cur:
            out[cur].add(line)
    return out
for kind in ("attrs",):
    a, b = blocks("/root/mig-before-attrs.txt"), blocks("/root/mig-after-attrs.txt")
    diff = {k: (sorted(a.get(k, set()) - b.get(k, set())), sorted(b.get(k, set()) - a.get(k, set()))) for k in set(a) | set(b)}
    diff = {k: v for k, v in diff.items() if v[0] or v[1]}
    print("per-file xattr/ACL differences:", diff or "none")
PY
cp /usr/bin/id /home/suidtest; chmod 4755 /home/suidtest
su -s /bin/bash nobody -c '/home/suidtest -u; id -u'; rm /home/suidtest
su ubuntu -c 'touch /home/ubuntu/mig/sub/new && getfacl -p /home/ubuntu/mig/sub/new | grep user:ubuntu'
rm -f /home/ubuntu/mig/sub/new
