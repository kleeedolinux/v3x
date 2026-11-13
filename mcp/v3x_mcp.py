#!/usr/bin/env python3
"""V3X MCP server: runs V3X console commands in the running game through plugins/devbridge.

Minimal MCP stdio server (JSON-RPC 2.0, one JSON message per line), Python standard library only.
Each tool call opens the devbridge named pipe, sends one request line "<id> <command>", reads the
output until "<id> END <rc>", and closes the pipe again (so nothing is held while idle).

Pipe: \\\\.\\pipe\\v3x_devbridge, or the name in env V3X_DEVBRIDGE_PIPE (same variable the plugin reads).
Timeout per call: env V3X_MCP_TIMEOUT seconds (default 60).
Register in Claude Code: see .mcp.json.example in the repo root.
"""
import ctypes
import json
import os
import sys
import time
from ctypes import wintypes

SERVER = {"name": "v3x", "version": "1.0.0"}
DEFAULT_PROTOCOL = "2024-11-05"
PIPE = "\\\\.\\pipe\\" + (os.environ.get("V3X_DEVBRIDGE_PIPE") or "v3x_devbridge").replace("\\", "_").replace("/", "_")
TIMEOUT = float(os.environ.get("V3X_MCP_TIMEOUT") or 60)
MAX_CMD = 4000  # the bridge accepts 4096-byte request lines

# ------------------------------------------------------------------ pipe client (ctypes, Win32)

GENERIC_READ, GENERIC_WRITE, OPEN_EXISTING = 0x80000000, 0x40000000, 3
ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY = 2, 231
INVALID_HANDLE = ctypes.c_void_p(-1).value

_k32 = ctypes.WinDLL("kernel32", use_last_error=True) if os.name == "nt" else None
if _k32:
    _k32.CreateFileW.restype = wintypes.HANDLE
    _k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.ReadFile.argtypes = _k32.WriteFile.argtypes
    _k32.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                   ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD)]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]


class BridgeError(Exception):
    pass


def _open_pipe(deadline):
    gone_after = time.monotonic() + 1.0
    while True:
        h = _k32.CreateFileW(PIPE, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if h and h != INVALID_HANDLE:
            return h
        err = ctypes.get_last_error()
        if err == ERROR_FILE_NOT_FOUND:
            # missing for good, or just between two clients (the bridge re-arms within milliseconds)
            if time.monotonic() > gone_after:
                raise BridgeError("V3X devbridge pipe %s not found: the game is not running, or the devbridge "
                                  "plugin is not installed / disabled (enabled=0 in v3x/devbridge.ini)." % PIPE)
        elif err != ERROR_PIPE_BUSY:
            raise BridgeError("cannot open %s (Windows error %d)" % (PIPE, err))
        if time.monotonic() > deadline:
            raise BridgeError("devbridge pipe busy (another client is connected) until the timeout")
        time.sleep(0.05)


def run_command(command, timeout=None):
    """Runs one console line in the game. Returns (rc, output text). Raises BridgeError."""
    if _k32 is None:
        raise BridgeError("V3X devbridge needs Windows (named pipe)")
    command = command.strip()
    if not command:
        raise BridgeError("empty command")
    if "\n" in command or "\r" in command:
        raise BridgeError("command must be a single line")
    data = command.encode("utf-8", "replace")
    if len(data) > MAX_CMD:
        raise BridgeError("command too long (max %d bytes)" % MAX_CMD)
    deadline = time.monotonic() + (timeout or TIMEOUT)
    h = _open_pipe(deadline)
    try:
        rid = "m%d" % (int(time.time() * 1000) % 100000000)
        req = rid.encode() + b" " + data + b"\n"
        w = wintypes.DWORD()
        if not _k32.WriteFile(h, req, len(req), ctypes.byref(w), None) or w.value != len(req):
            raise BridgeError("write to devbridge failed (Windows error %d)" % ctypes.get_last_error())
        marker = b"\n" + rid.encode() + b" END "
        buf = bytearray(b"\n")
        chunk = ctypes.create_string_buffer(65536)
        avail, got = wintypes.DWORD(), wintypes.DWORD()
        while True:
            i = buf.find(marker)
            if i >= 0:
                j = buf.find(b"\n", i + len(marker))
                if j >= 0:
                    rc = int(buf[i + len(marker):j].strip() or b"-999")
                    return rc, bytes(buf[1:i + 1]).decode("utf-8", "replace")
            if not _k32.PeekNamedPipe(h, None, 0, None, ctypes.byref(avail), None):
                raise BridgeError("devbridge closed the connection (the game exited?)")
            if not avail.value:
                if time.monotonic() > deadline:
                    raise BridgeError("timeout after %.0f s waiting for '%s' (game busy or command hung?); "
                                      "partial output:\n%s" % (timeout or TIMEOUT, command, bytes(buf[1:]).decode("utf-8", "replace")))
                time.sleep(0.01)
                continue
            n = min(avail.value, len(chunk))
            if not _k32.ReadFile(h, chunk, n, ctypes.byref(got), None):
                raise BridgeError("read from devbridge failed (Windows error %d)" % ctypes.get_last_error())
            buf += chunk.raw[:got.value]
    finally:
        _k32.CloseHandle(h)


# ------------------------------------------------------------------ tools


def _s(desc):
    return {"type": "string", "description": desc}


def _i(desc):
    return {"type": "integer", "description": desc}


ADDR = "address: hex absolute (0x7ff6...), +RVA in the game exe (+0x3e17820), or module+offset as printed (victoria3.exe+0x10)"

TOOLS = [
    ("v3x_exec", "Run any V3X console command line in the running game and return its text output "
     "(e.g. 'help', 'plugins', 'scan 48 8B 05 ?? ?? ?? ??', 'mods', 'packs', 'memcollect'). Run 'help' to list commands.",
     {"command": _s("one console line, e.g. 'dis +0x1000 20'")}, ["command"],
     lambda a: a["command"]),
    ("v3x_dis", "Disassemble instructions at an address in the game process (console 'dis').",
     {"addr": _s(ADDR), "count": _i("number of instructions (default 20, max 500)")}, ["addr"],
     lambda a: "dis %s%s" % (a["addr"], " %d" % int(a["count"]) if a.get("count") is not None else "")),
    ("v3x_fn", "Disassemble the whole function containing an address, bounds from .pdata (console 'fn').",
     {"addr": _s(ADDR)}, ["addr"],
     lambda a: "fn %s" % a["addr"]),
    ("v3x_xref", "List rip-relative lea/mov in the game's .text that reference an address or an exact string (console 'xref').",
     {"target": _s(ADDR + "; or an exact string when is_string is true"),
      "is_string": {"type": "boolean", "description": "treat target as an exact string (quoted for you)"},
      "max": _i("max results")}, ["target"],
     lambda a: "xref %s%s" % ('"%s"' % a["target"] if a.get("is_string") else a["target"],
                              " %d" % int(a["max"]) if a.get("max") is not None else "")),
    ("v3x_mem", "Hex + ASCII dump of game memory (console 'mem'; unreadable memory prints an error, never crashes).",
     {"addr": _s(ADDR), "len": _i("bytes (default 128, max 4096)")}, ["addr"],
     lambda a: "mem %s%s" % (a["addr"], " %d" % int(a["len"]) if a.get("len") is not None else "")),
    ("v3x_profile_report", "devtools sampling profiler report (console 'profile report').", {}, [],
     lambda a: "profile report"),
    ("v3x_ticks", "tick_profiler: per-tick-task timing profile (console 'ticks').", {}, [],
     lambda a: "ticks"),
]
TOOL_MAP = {t[0]: t for t in TOOLS}


def tools_list():
    return [{"name": n, "description": d,
             "inputSchema": {"type": "object", "properties": props, "required": req}}
            for n, d, props, req, _ in TOOLS]


def call_tool(name, args):
    t = TOOL_MAP.get(name)
    if not t:
        return {"content": [{"type": "text", "text": "unknown tool '%s'" % name}], "isError": True}
    args = args or {}
    try:
        for r in t[3]:
            if args.get(r) in (None, ""):
                raise BridgeError("missing argument '%s'" % r)
        for k in ("addr", "target"):
            if k in args and not isinstance(args[k], str):
                args[k] = str(args[k])
        if name == "v3x_xref" and args.get("is_string") and '"' in args["target"]:
            raise BridgeError('string target must not contain a double quote')
        line = t[4](args)
        rc, out = run_command(line)
    except (BridgeError, ValueError, TypeError) as e:
        return {"content": [{"type": "text", "text": "error: %s" % e}], "isError": True}
    text = out if out else "(no output)\n"
    if rc != 0:
        text += "[rc %d: %s]\n" % (rc, "unknown command" if rc == -1 else "bridge error")
    return {"content": [{"type": "text", "text": text}], "isError": rc != 0}


# ------------------------------------------------------------------ JSON-RPC over stdio


def handle(msg):
    """Returns the response dict, or None for notifications."""
    mid, method, params = msg.get("id"), msg.get("method"), msg.get("params") or {}
    if mid is None:
        return None  # notification (notifications/initialized, cancelled, ...)
    if method == "initialize":
        result = {"protocolVersion": params.get("protocolVersion") or DEFAULT_PROTOCOL,
                  "capabilities": {"tools": {}}, "serverInfo": SERVER,
                  "instructions": "Runs V3X console commands inside the running Victoria 3 (via the devbridge plugin)."}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": tools_list()}
    elif method == "tools/call":
        try:
            result = call_tool(params.get("name"), params.get("arguments"))
        except Exception as e:  # never let one bad call kill the server
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": "internal error: %r" % e}}
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found: %s" % method}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    stdin = sys.stdin.buffer
    out = sys.stdout.buffer
    for raw in stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except ValueError:
            resp = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
        else:
            if isinstance(msg, list):  # batch
                resp = [r for r in (handle(m) for m in msg if isinstance(m, dict)) if r] or None
            elif isinstance(msg, dict):
                resp = handle(msg)
            else:
                resp = {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "invalid request"}}
        if resp is not None:
            out.write(json.dumps(resp, ensure_ascii=False).encode("utf-8") + b"\n")
            out.flush()


if __name__ == "__main__":
    main()
