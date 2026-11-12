#!/usr/bin/env python3
"""trace.py SESSION.jsonl [CUT]: what the agent did in one engine conversation (the decompressed
session.v4.jsonl): tool calls, their results (cut to CUT characters, default 600), its replies and
how each turn ended. Run by trace.sh."""
import json, sys

cut = int(sys.argv[2]) if len(sys.argv) > 2 else 600
for line in open(sys.argv[1], encoding="utf-8"):
    o = json.loads(line)
    t, d = o.get("type"), o.get("data", {})
    if t == "user/message":
        m = d.get("message", d)
        text = "".join(c.get("text", "") for c in m.get("content", []) if isinstance(c, dict))
        if text.startswith("<system-reminder>"):  # what the engine adds (AGENTS.md, the time)
            print(f"\n>>> (engine context, {len(text)} chars)")
        elif text:
            print("\n>>> USER: " + text)
    elif t == "tool/call":
        a = d.get("arguments", "")
        try:
            a = json.loads(a)
        except (TypeError, ValueError):
            pass
        body = a["command"] if isinstance(a, dict) and "command" in a else json.dumps(a, ensure_ascii=False)
        print(f"\n=== [{d.get('name')}] turn {d.get('turn')} step {d.get('step')}\n{body}")
    elif t == "tool/result":
        m = d.get("message", {})
        text = "".join(c.get("text", "") for c in m.get("content", []) if isinstance(c, dict))
        print(("--- ERROR " if m.get("isError") else "--- ") + text.replace("\r", "")[:cut])
    elif t == "assistant/message":
        for c in d.get("message", {}).get("content", []):
            if c.get("type") == "reasoning" and c.get("text"):
                print("\n# think: " + c["text"][:cut].replace("\n", " "))
            elif c.get("type") == "text":
                print("\n# reply: " + c["text"])
    elif t == "turn/end":
        print(f"\n*** turn {d.get('turn')} ended: {json.dumps(d.get('reason'), ensure_ascii=False)}")
