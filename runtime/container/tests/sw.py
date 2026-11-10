#!/usr/bin/env python3
"""sw.py LABEL [on|off]: prints a switch's state, or flips it to the wanted one."""
import os, re, subprocess, sys, xml.etree.ElementTree as ET
ADB = [os.path.expanduser("~/android-sdk/platform-tools/adb")]
def find():
    subprocess.run(ADB + ["shell", "uiautomator dump /data/local/tmp/ui.xml >/dev/null"], check=True)
    root = ET.fromstring(subprocess.run(ADB + ["exec-out", "cat /data/local/tmp/ui.xml"], capture_output=True).stdout)
    for n in root.iter("node"):
        t = (n.get("content-desc") or "") + (n.get("text") or "")
        if n.get("checkable") == "true" and t.startswith(sys.argv[1]):
            return n
n = find()
if n is None: sys.exit("switch not found")
print("now:", n.get("checked"))
if len(sys.argv) > 2 and (n.get("checked") == "true") != (sys.argv[2] == "on"):
    b = [int(x) for x in re.findall(r"\d+", n.get("bounds"))]
    subprocess.run(ADB + ["shell", f"input tap {(b[0]+b[2])//2} {(b[1]+b[3])//2}"], check=True)
    print("after:", find().get("checked"))
