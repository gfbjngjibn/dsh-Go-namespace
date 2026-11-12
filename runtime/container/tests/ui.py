#!/usr/bin/env python3
"""ui.py dump | tap TEXT [--nth N] | tapx TEXT [--nth N] | has TEXT : uiautomator helpers over adb. tap and has match a substring of text|content-desc (a line break shows as " / " in dump but is "\n" in the node); tapx matches the whole text exactly. Prefer tapx: a substring can hit a whole section and tap its centre.
Every command dumps, and a dump unbinds the app's accessibility service for about a second, which stops
a turn that uses phone control: while the agent runs, watch it with trace.sh instead."""
import re, subprocess, sys, time, xml.etree.ElementTree as ET
ADB = [__import__("os").path.expanduser("~/android-sdk/platform-tools/adb")]
def dump():
    subprocess.run(ADB + ["shell", "uiautomator dump /data/local/tmp/ui.xml >/dev/null"], check=True)
    xml = subprocess.run(ADB + ["exec-out", "cat /data/local/tmp/ui.xml"], capture_output=True, check=True).stdout
    return ET.fromstring(xml)
def nodes(root):
    for n in root.iter("node"):
        t = (n.get("text") or "") + "|" + (n.get("content-desc") or "")
        b = [int(x) for x in re.findall(r"\d+", n.get("bounds"))]
        yield t, b, n
cmd = sys.argv[1]
if cmd == "dump":
    for t, b, n in nodes(dump()):
        if t != "|":
            print(b, t.replace("\n", " / ")[:160], "[click]" if n.get("clickable") == "true" else "", "[off]" if n.get("enabled") == "false" else "")
elif cmd in ("tap", "has", "tapx"):
    want = sys.argv[2]; nth = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[3] == "--nth" else 0
    hits = [(t, b) for t, b, n in nodes(dump()) if (t.strip("|").strip() == want if cmd == "tapx" else want in t)]
    if not hits:
        print("NOT FOUND:", want); sys.exit(1)
    t, b = hits[nth]
    print("found:", t.replace("\n", " / ")[:120], b)
    if cmd in ("tap", "tapx"):
        subprocess.run(ADB + ["shell", f"input tap {(b[0]+b[2])//2} {(b[1]+b[3])//2}"], check=True)
