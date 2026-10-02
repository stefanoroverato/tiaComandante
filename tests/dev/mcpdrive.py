"""Development helper (optional, needs Python 3): drives tiacomandante over MCP stdio
in a single session, so TIA Portal is attached only once.

    python tests/dev/mcpdrive.py - TOOL '{json args}' [TOOL '{json args}' ...]
    python tests/dev/mcpdrive.py calls.json      # [["tool", {...}], ...]

Set PYTHONIOENCODING=utf-8 for correct console output."""
import json
import re
import os
import subprocess
import sys
import time

EXE = os.environ.get("TIACMD_EXE", os.path.join(os.path.dirname(__file__), "..", "..", "build", "Release",
                                                 "tiacomandante.exe"))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "-":
        a = sys.argv[2:]
        calls = [(a[i], json.loads(a[i + 1])) for i in range(0, len(a), 2)]
    else:
        text = "[" + ",".join(open(fn, encoding="utf-8").read().strip()[1:-1] for fn in sys.argv[1:]) + "]"
        # ${TS} and ${ENV_VAR} placeholders (JSON-escaped) for reusable scenario files,
        # e.g. ${TEMP}, ${PLC_IP}, ${PLC_DEVICE}, ${PLC_PCIF}.
        text = text.replace("${TS}", time.strftime("%H%M%S"))
        def env(m):
            v = os.environ.get(m.group(1))
            if v is None:
                sys.exit(f"environment variable {m.group(1)} is not set (used by the scenario)")
            return json.dumps(v)[1:-1]
        text = re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", env, text)
        calls = json.loads(text)
    p = subprocess.Popen([EXE, "--log-level", "warn"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=sys.stderr, bufsize=0)

    def send(obj):
        p.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
        p.stdin.flush()

    def recv():
        line = p.stdout.readline()
        return json.loads(line) if line else None

    send({"jsonrpc": "2.0", "id": 0, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                     "clientInfo": {"name": "mcpdrive", "version": "1"}}})
    recv()
    failures = 0
    for i, (tool, args) in enumerate(calls, 1):
        expect_error = isinstance(args, dict) and args.pop("_expectError", False)
        t0 = time.time()
        send({"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {"name": tool, "arguments": args}})
        while True:
            m = recv()
            if m is None:
                print("!! server closed")
                return 1
            if m.get("id") == i:
                break
        r = m.get("result")
        print(f"===== {tool} {json.dumps(args)}  ({time.time() - t0:.1f}s)")
        if r:
            print(("[ERROR] " if r.get("isError") else "") + r["content"][0]["text"].rstrip())
            if bool(r.get("isError")) != bool(expect_error):
                failures += 1
                if not os.environ.get("MCPDRIVE_CONTINUE"):
                    print("!! stopping at the first unexpected result (set MCPDRIVE_CONTINUE=1 to go on)")
                    break
        else:
            failures += 1
            print(json.dumps(m))
            break
    p.stdin.close()
    p.wait(60)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
