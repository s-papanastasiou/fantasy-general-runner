#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a Ghidra script from tools/ghidra/scripts/ in the running Ghidra GUI.

    tools/ghidra/run.py FGIntInventory.java [args...]

Talks to the GhidraMCP plugin's HTTP API (started by tools/ghidra.sh) with the
token from tools/vendor/ghidra-mcp.token, and prints the script's output.
The script's changes go into the program open in Ghidra; save from Ghidra
(or with --save) to keep them.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
URL = os.environ.get("GHIDRA_MCP_URL", "http://127.0.0.1:8089")


def call(endpoint, payload, token, timeout):
    req = urllib.request.Request(
        URL + endpoint, data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", help="file name in tools/ghidra/scripts/ (or a path)")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--save", action="store_true", help="save the program afterwards")
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args()

    path = a.script if os.path.sep in a.script else os.path.join(ROOT, "tools/ghidra/scripts", a.script)
    with open(path) as f:
        code = f.read()
    with open(os.path.join(ROOT, "tools/vendor/ghidra-mcp.token")) as f:
        token = f.read().strip()

    try:
        res = call("/run_script_inline", {"code": code, "args": " ".join(a.args)}, token, a.timeout)
    except urllib.error.HTTPError as e:
        sys.exit("HTTP %d: %s" % (e.code, e.read().decode()[:2000]))
    except urllib.error.URLError as e:
        sys.exit("cannot reach Ghidra at %s (%s); start it with tools/ghidra.sh" % (URL, e.reason))

    out = res.get("console_output", "")
    # Strip the plugin's banner, keep the script's own output.
    if "--- SCRIPT OUTPUT ---" in out:
        out = out.split("--- SCRIPT OUTPUT ---", 1)[1]
        out = out.rsplit("=== SCRIPT", 1)[0]
    sys.stdout.write(out.strip("\n") + "\n")
    if not res.get("success", False):
        sys.exit(1)
    if a.save:
        call("/save_program", {}, token, 120)
        print("[saved]")


if __name__ == "__main__":
    main()
