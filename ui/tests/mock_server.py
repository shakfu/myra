"""Scripted chat-completions server for myra-ui's smoke test and manual runs.

Turn 1 thinks, then runs `ls`; turn 2 edits notes.txt; turn 3 answers in markdown.
Prints the port it listens on, then serves until killed. Usage: mock_server.py [port]
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def events(deltas, finish):
    out = [{"choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}]}]
    out += [{"choices": [{"index": 0, "delta": d, "finish_reason": None}]} for d in deltas]
    out.append({"choices": [{"index": 0, "delta": {}, "finish_reason": finish}]})
    out.append({"choices": [], "usage": {"prompt_tokens": 1200, "completion_tokens": 80}})
    return out


def words(field, text):
    return [{field: w + " "} for w in text.split(" ")]


def call(i, name, args):
    return [{"tool_calls": [{"index": 0, "id": f"c{i}", "type": "function",
                             "function": {"name": name, "arguments": json.dumps(args)}}]}]


def reply(body):
    done = sum(1 for m in body["messages"] if m["role"] == "tool")
    if done == 0:
        return events(words("reasoning", "The user wants a summary, so list the files first.")
                      + words("content", "Let me look at the folder.")
                      + call(1, "shell", {"command": "ls"}), "tool_calls")
    if done == 1:
        return events(words("content", "I will tidy the notes.")
                      + call(2, "edit", {"path": "notes.txt", "old_string": "hello",
                                         "new_string": "hello, world"}), "tool_calls")
    return events(words("content", "Done. The folder has **one file**, `notes.txt`:\n\n"
                        "- `ls` listed it\n- `edit` changed its greeting"), "stop")


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for e in reply(body):
            self.wfile.write(("data: " + json.dumps(e) + "\n\n").encode())
            self.wfile.flush()
            time.sleep(0.02)
        self.wfile.write(b"data: [DONE]\n\n")

    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(json.dumps({"data": [{"id": "mock-small"}, {"id": "mock-large"}]}).encode())

    def log_message(self, *a):
        pass


server = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
