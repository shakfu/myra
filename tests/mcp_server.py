"""A stdio MCP server for tests/test_agent.py. Each message it receives is appended
to $MCP_LOG as one JSON line, and {"eof": true} when stdin closes.

With $MCP_HANG set, initialize is never answered; with $MCP_WAIT, it is answered
once that file exists. With $MCP_LIST_FAIL, the server exits instead of sending page 2.
With $MCP_ENV_LOG, it writes its environment there at start. With $MCP_DEAF, slow stops
reading stdin. With $MCP_STUBBORN, it outlives EOF and ignores SIGTERM.

Tools: echo (asks the client for a ping first, then returns its text and
$MCP_GREETING), fail (isError), image (non-text content) and slow (30 s; a cancel
ends it). tools/list answers in two pages."""
import json
import os
import signal
import sys
import time

TOOLS = [[{"name": "echo", "description": "Echo text.",
           "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}}},
          {"name": "fail"}],
         [{"name": "image"}, {"name": "slow"}, {"name": "bad name!"}]]


def send(msg):
    msg["jsonrpc"] = "2.0"
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def recv():
    line = sys.stdin.readline()
    msg = json.loads(line) if line else {"eof": True}
    with open(os.environ["MCP_LOG"], "a") as f:
        f.write(json.dumps(msg) + "\n")
    if not line:
        if os.environ.get("MCP_STUBBORN"):
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            time.sleep(60)
        sys.exit(0)
    return msg


def result(rid, r):
    send({"id": rid, "result": r})


if os.environ.get("MCP_ENV_LOG"):
    with open(os.environ["MCP_ENV_LOG"], "w") as f:
        json.dump(dict(os.environ), f)
while True:
    m = recv()
    method, rid, p = m.get("method"), m.get("id"), m.get("params", {})
    if method == "initialize":
        if os.environ.get("MCP_HANG"):
            recv()  # until stdin closes
        while (wait := os.environ.get("MCP_WAIT")) and not os.path.exists(wait):
            time.sleep(0.05)
        result(rid, {"protocolVersion": p["protocolVersion"], "capabilities": {"tools": {}},
                     "serverInfo": {"name": "fake", "version": "1"}})
    elif method == "tools/list":
        page = int(p.get("cursor", "0"))
        if page and os.environ.get("MCP_LIST_FAIL"):
            sys.exit(1)
        r = {"tools": TOOLS[page]}
        if page + 1 < len(TOOLS):
            r["nextCursor"] = str(page + 1)
        result(rid, r)
    elif method == "tools/call":
        name, args = p["name"], p.get("arguments", {})
        if name == "echo":
            send({"id": "ping-1", "method": "ping"})
            assert recv() == {"jsonrpc": "2.0", "id": "ping-1", "result": {}}
            text = f"{args.get('text')} {os.environ.get('MCP_GREETING', '')}"
            result(rid, {"content": [{"type": "text", "text": text}]})
        elif name == "fail":
            result(rid, {"content": [{"type": "text", "text": "it broke"}], "isError": True})
        elif name == "image":
            result(rid, {"content": [{"type": "image", "data": "", "mimeType": "image/png"},
                                     {"type": "resource_link", "uri": "file:///x", "name": "x"}]})
        elif name == "slow":
            if os.environ.get("MCP_DEAF"):
                time.sleep(3600)
            recv()  # blocks until the client's notifications/cancelled, logged
