"""Scripted chat-completions server for myra-swarm's tests.

Each request's first user message is a JSON script, or holds one as its last line that
parses as a JSON object, outside <answer> elements:
  {"calls": [[TOOL, ARGS], ...], "say": TEXT, "delay": SECONDS, "status": CODE, "echo": BOOL,
   "whoami": BOOL}
The server answers with the next call in order. Once every call has a result, it answers
TEXT, then each tool result on its own line, then with "echo" the user message, then with
"whoami" "agent NAME" for the family member the system prompt names, or "agent -". "delay" waits before each answer. "status"
fails every request with that HTTP code and an error naming TEXT. A message that is not
JSON gets "ok". GET /_log lists when each request arrived and its script's "say". Prints its port, then
serves until killed. Usage: mock.py
"""
import json
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def events(delta, finish):
    return [
        {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
        {"choices": [{"index": 0, "delta": {}, "finish_reason": finish}]},
        {"choices": [], "usage": {"prompt_tokens": 100, "completion_tokens": 10}},
    ]


def script(body):
    """The whole first user message as a script, else its last line that is one, outside
    any <answer> element: those quote other agents' prompts in myra-swarm's combiner."""
    user = next(m["content"] for m in body["messages"] if m["role"] == "user")
    own = re.sub(r"<answer[^>]*>.*?</answer>", "", user, flags=re.DOTALL)
    for text in [user] + own.splitlines()[::-1]:
        try:
            s = json.loads(text)
        except ValueError:
            continue
        if isinstance(s, dict):
            return s, user
    return {}, user


LOG = []  # (monotonic seconds, the script's "say"), one per request, for GET /_log


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):  # /_log: when each request arrived, to check what ran in parallel
        data = json.dumps(LOG).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        s, user = script(body)
        LOG.append((time.monotonic(), s.get("say")))
        time.sleep(s.get("delay", 0))
        if s.get("status"):
            err = json.dumps({"error": {"message": "fail-" + s.get("say", "")}}).encode()
            self.send_response(s["status"])
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.end_headers()
            self.wfile.write(err)
            return
        results = [m["content"] for m in body["messages"] if m["role"] == "tool"]
        calls = s.get("calls", [])
        if len(results) < len(calls):
            name, args = calls[len(results)]
            i = len(results)
            delta = {"tool_calls": [{"index": 0, "id": f"c{i}", "type": "function",
                                     "function": {"name": name, "arguments": json.dumps(args)}}]}
            out = events(delta, "tool_calls")
        else:
            who = re.search(r'You are agent "([^"]+)"', body["messages"][0]["content"] or "")
            text = "\n".join([s.get("say", "ok")] + results + ([user] if s.get("echo") else [])
                              + ([f"agent {who[1] if who else '-'}"] if s.get("whoami") else []))
            out = events({"content": text}, "stop")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        for e in out:
            self.wfile.write(("data: " + json.dumps(e) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")
        self.close_connection = True

    def log_message(self, *a):
        pass


class Server(ThreadingHTTPServer):
    request_queue_size = 256  # the default, 5, resets connects from a large swarm


if __name__ == "__main__":
    server = Server(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    print(server.server_address[1], flush=True)
    server.serve_forever()
