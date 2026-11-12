#!/bin/bash
# trace.sh [N [CUT]]: what the agent did in the newest conversation (N=2: the one before, ...) of the
# engine in the user's system container, from its session record, without touching the screen.
# (Polling the screen with `ui.py dump` while the agent uses phone control aborts its turn: see README.)
set -e
H=$(cd "$(dirname "$0")" && pwd)
N=${1:-1}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/get.sh" <<EOF
F=\$(ls -t /root/.dsh/sessions/*/session-*/session.v4.jsonl.zstd | sed -n ${N}p)
echo "FILE \$F"
echo BEGIN; base64 -w0 "\$F"; echo; echo END
EOF
"$H/cx.sh" "$T/get.sh" 60 > "$T/out"
grep '^FILE ' "$T/out"
sed -n '/^BEGIN$/,/^END$/p' "$T/out" | sed '1d;$d' | base64 -d | zstd -dc > "$T/s.jsonl"
python3 -I "$H/trace.py" "$T/s.jsonl" "${2:-600}"
