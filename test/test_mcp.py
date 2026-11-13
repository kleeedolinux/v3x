#!/usr/bin/env python3
"""Tests mcp/v3x_mcp.py against a live devbridge, without the game.

Starts devbridge_test.exe --serve N (set up as described in test/devbridge_test.c) on a private pipe,
runs v3x_mcp.py as a subprocess and checks initialize / tools/list / tools/call, plus the
"game not running" error. Exit code 0 = ALL OK.

Usage: python test/test_mcp.py <dir>/devbridge_test.exe
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MCP = os.path.join(HERE, "..", "mcp", "v3x_mcp.py")
fails = 0


def check(ok, what):
    global fails
    print("%-64s %s" % (what, "OK" if ok else "FAIL"))
    fails += not ok


class Client:
    def __init__(self, env):
        self.p = subprocess.Popen([sys.executable, "-I", MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.n = 0

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.n += 1
            msg["id"] = self.n
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        if notify:
            return None
        return json.loads(self.p.stdout.readline())

    def call(self, name, args):
        r = self.send("tools/call", {"name": name, "arguments": args})["result"]
        return r.get("isError"), r["content"][0]["text"]

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(10)
        return rc


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    pipe = "v3x_devbridge_mcptest_%d" % os.getpid()
    env = dict(os.environ, V3X_NO_CONSOLE="1", V3X_DEVBRIDGE_PIPE=pipe, V3X_MCP_TIMEOUT="20")
    game = subprocess.Popen([exe, "--serve", "60"], stdout=subprocess.PIPE, env=env)
    try:
        ready = game.stdout.readline().decode(errors="replace")
        check(ready.startswith("READY \\\\.\\pipe\\" + pipe), "devbridge_test --serve: READY on the private pipe")

        c = Client(env)
        r = c.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                  "clientInfo": {"name": "test", "version": "0"}})
        res = r.get("result", {})
        check(r.get("id") == 1 and res.get("protocolVersion") == "2025-06-18" and "tools" in res.get("capabilities", {})
              and res.get("serverInfo", {}).get("name") == "v3x", "initialize: version echoed, tools capability")
        check(c.send("notifications/initialized", notify=True) is None, "notifications/initialized: no reply")
        r = c.send("tools/list")
        names = [t["name"] for t in r["result"]["tools"]]
        want = ["v3x_exec", "v3x_dis", "v3x_fn", "v3x_xref", "v3x_mem", "v3x_profile_report", "v3x_ticks"]
        check(names == want and all(t["inputSchema"]["type"] == "object" for t in r["result"]["tools"]),
              "tools/list: 7 tools with input schemas")

        err, text = c.call("v3x_exec", {"command": "help"})
        check(not err and "  dis " in text and "  help" in text, "tools/call v3x_exec help")
        err, text = c.call("v3x_dis", {"addr": "+1000", "count": 3})
        exe_name = os.path.basename(exe)
        check(not err and text.count("\n") == 3 and (exe_name + "+0x1000") in text, "tools/call v3x_dis +1000 3 -> 3 lines")
        err, text = c.call("v3x_mem", {"addr": "+1000", "len": 32})
        check(not err and (exe_name + "+0x1000") in text and text.count("\n") == 2, "tools/call v3x_mem +1000 32 -> 2 rows")
        err, text = c.call("v3x_fn", {"addr": "0x10"})
        check(not err and "no .pdata entry" in text, "tools/call v3x_fn 0x10 -> command's own error text")
        err, text = c.call("v3x_xref", {"target": "no such string zz9", "is_string": True})
        check(not err and "not found" in text, "tools/call v3x_xref string (quoted for the console)")
        err, text = c.call("v3x_ticks", {})
        check(err and "unknown command 'ticks'" in text, "v3x_ticks without tick_profiler -> isError, message")
        err, text = c.call("v3x_exec", {"command": "help\nplugins"})
        check(err and "single line" in text, "multi-line command rejected")
        err, text = c.call("v3x_dis", {})
        check(err and "missing argument 'addr'" in text, "missing required argument -> isError")
        r = c.send("no/such/method")
        check(r.get("error", {}).get("code") == -32601, "unknown method -> -32601")
        r = c.send("ping")
        check(r.get("result") == {}, "ping -> {}")
        check(c.close() == 0, "server exits cleanly on stdin EOF")

        c = Client(dict(env, V3X_DEVBRIDGE_PIPE=pipe + "_nobody"))
        c.send("initialize", {})
        err, text = c.call("v3x_exec", {"command": "help"})
        check(err and "game is not running" in text, "no pipe -> clear 'game is not running' error")
        c.close()
    finally:
        game.kill()
        game.wait(10)
    print("FAILED" if fails else "ALL OK")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
