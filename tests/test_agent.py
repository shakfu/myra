"""End-to-end tests: run ./agent against a scripted mock of each provider's API."""

import json
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

AGENT = Path(__file__).resolve().parent.parent / "agent"

# provider -> (key env, base-url env, base-url suffix, expected path, extra args)
PROVIDERS = {
    "anthropic": ("ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "", "/v1/messages", []),
    "openrouter": ("OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "/api/v1",
                   "/api/v1/chat/completions", []),
    "openai": ("OPENAI_API_KEY", "OPENAI_BASE_URL", "/v1", "/v1/chat/completions",
               ["-m", "gpt-test"]),
    "compat": ("COMPAT_API_KEY", "COMPAT_BASE_URL", "/v1", "/v1/chat/completions", []),
}


class Mock:
    """Serves queued (status, body) replies and records each request."""

    def __init__(self):
        self.replies, self.requests = [], []
        mock = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["content-length"]))
                mock.requests.append({"path": self.path,
                                      "headers": {k.lower(): v for k, v in self.headers.items()},
                                      "body": json.loads(body)})
                status, reply = mock.replies.pop(0)
                data = json.dumps(reply).encode()
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"


class Api:
    """Builds replies and reads tool results in one provider's wire format."""

    def __init__(self, mock, provider):
        self.mock, self.provider = mock, provider
        self.anthropic = provider == "anthropic"

    def env(self, key="test-key"):
        key_env, base_env, suffix, _, _ = PROVIDERS[self.provider]
        env = {"PATH": "/usr/bin:/bin", base_env: self.mock.url + suffix}
        if key is not None:
            env[key_env] = key
        return env

    def run(self, cwd, *args, stdin="", key="test-key"):
        # New session: no controlling terminal, so /dev/tty confirmation cannot block.
        argv = [str(AGENT), "-P", self.provider, *PROVIDERS[self.provider][4], *args]
        return subprocess.run(argv, cwd=cwd, env=self.env(key), input=stdin, text=True,
                              capture_output=True, timeout=60, start_new_session=True)

    def reply(self, text=None, calls=(), stop=None):
        """calls: (id, name, input dict or raw argument string)."""
        if self.anthropic:
            content = ([{"type": "text", "text": text}] if text else []) + [
                {"type": "tool_use", "id": i, "name": n, "input": a} for i, n, a in calls]
            stop = stop or ("tool_use" if calls else "end_turn")
            body = {"type": "message", "role": "assistant", "content": content, "stop_reason": stop}
        else:
            msg = {"role": "assistant", "content": text}
            if calls:
                msg["tool_calls"] = [{"id": i, "type": "function", "function": {
                    "name": n, "arguments": a if isinstance(a, str) else json.dumps(a)}}
                    for i, n, a in calls]
            stop = stop or ("tool_calls" if calls else "stop")
            body = {"choices": [{"index": 0, "message": msg, "finish_reason": stop}]}
        self.mock.replies.append((200, body))

    def results(self, req):
        """{tool id: (content, is_error)} for the results sent in req."""
        msgs = req["body"]["messages"]
        if self.anthropic:
            assert msgs[-1]["role"] == "user"
            return {r["tool_use_id"]: (r["content"], r.get("is_error", False))
                    for r in msgs[-1]["content"]}
        out = {}
        for m in reversed(msgs):
            if m["role"] != "tool":
                break
            err = m["content"].startswith("error: ")
            out[m["tool_call_id"]] = (m["content"].removeprefix("error: "), err)
        return out


@pytest.fixture
def mock():
    m = Mock()
    yield m
    m.server.shutdown()


@pytest.fixture(params=list(PROVIDERS))
def api(request, mock):
    return Api(mock, request.param)


# ---- behaviour shared by every provider ----

def test_request_path_and_tools(api, tmp_path):
    api.reply("hello")
    p = api.run(tmp_path, "-p", "say hi")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "hello\n"
    req = api.mock.requests[0]
    assert req["path"] == PROVIDERS[api.provider][3]
    tools = req["body"]["tools"]
    names = [t["name"] if api.anthropic else t["function"]["name"] for t in tools]
    assert names == ["read", "write", "edit", "shell"]


def test_all_four_tools_in_one_turn(api, tmp_path):
    api.reply("working", [("t1", "write", {"path": "a.txt", "content": "one\ntwo\n"}),
                          ("t2", "edit", {"path": "a.txt", "old_string": "two", "new_string": "2"}),
                          ("t3", "read", {"path": "a.txt"}),
                          ("t4", "shell", {"command": "wc -l < a.txt; exit 3 # comment"})])
    api.reply("done")
    p = api.run(tmp_path, "-y", "-p", "go")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "working\ndone\n"
    assert (tmp_path / "a.txt").read_text() == "one\n2\n"
    r = api.results(api.mock.requests[1])
    assert set(r) == {"t1", "t2", "t3", "t4"}
    assert not r["t1"][1] and not r["t2"][1]
    assert r["t3"] == ("one\n2\n", False)
    assert r["t4"][0].split() == ["2", "[exit", "3]"] and r["t4"][1]


@pytest.mark.parametrize("inp,msg", [
    ({"path": "b.txt", "old_string": "x", "new_string": "y"}, "more than once"),
    ({"path": "b.txt", "old_string": "zzz", "new_string": "y"}, "not found"),
    ({"path": "b.txt", "old_string": "", "new_string": "y"}, "empty"),
    ({"path": "missing.txt", "old_string": "x", "new_string": "y"}, "cannot read"),
    ({"path": "b.txt"}, "missing arguments"),
])
def test_edit_errors(api, tmp_path, inp, msg):
    (tmp_path / "b.txt").write_text("x x\n")
    api.reply(calls=[("e", "edit", inp)])
    api.reply("ok")
    p = api.run(tmp_path, "-y", "-p", "go")
    assert p.returncode == 0, p.stderr
    content, err = api.results(api.mock.requests[1])["e"]
    assert err and msg in content
    assert (tmp_path / "b.txt").read_text() == "x x\n"


def test_read_errors(api, tmp_path):
    (tmp_path / "bin").write_bytes(b"a\0b")
    api.reply(calls=[("r1", "read", {"path": "bin"}), ("r2", "read", {"path": "nope"}),
                     ("r3", "bogus", {})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    r = api.results(api.mock.requests[1])
    assert r["r1"][1] and "binary" in r["r1"][0]
    assert r["r2"][1] and "cannot read" in r["r2"][0]
    assert r["r3"][1] and "unknown tool" in r["r3"][0]


def test_shell_output_is_capped(api, tmp_path):
    api.reply(calls=[("s", "shell", {"command": "head -c 300000 /dev/zero | tr '\\0' a"})])
    api.reply("ok")
    api.run(tmp_path, "-y", "-p", "go")
    out = api.results(api.mock.requests[1])["s"][0]
    assert out.startswith("a" * 102400 + "\n[truncated: 300000 bytes total")
    assert out.endswith("[exit 0]")


def test_mutating_tools_denied_without_tty(api, tmp_path):
    (tmp_path / "e.txt").write_text("readable")
    api.reply(calls=[("w", "write", {"path": "c.txt", "content": "x"}),
                     ("s", "shell", {"command": "touch d.txt"}),
                     ("r", "read", {"path": "e.txt"})])
    api.reply("ok")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    r = api.results(api.mock.requests[1])
    assert r["w"] == ("denied by user", True)
    assert r["s"] == ("denied by user", True)
    assert r["r"] == ("readable", False)  # read needs no confirmation
    assert not (tmp_path / "c.txt").exists() and not (tmp_path / "d.txt").exists()


def test_retries_overloaded_then_succeeds(api, tmp_path):
    api.mock.replies.append((529, {"error": {"type": "overloaded_error"}}))
    api.reply("recovered")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "recovered\n"
    assert len(api.mock.requests) == 2 and "retrying" in p.stderr


def test_client_error_not_retried(api, tmp_path):
    api.mock.replies.append((400, {"error": {"message": "bad request"}}))
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 1 and "bad request" in p.stderr
    assert len(api.mock.requests) == 1


def test_repl_keeps_history_and_rolls_back_failures(api, tmp_path):
    api.reply("first")
    api.mock.replies.append((400, {"error": {"message": "nope"}}))
    api.reply("second")
    p = api.run(tmp_path, stdin="one\n\nbad\ntwo\n/exit\nnever\n")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "first\nsecond\n"
    msgs = api.mock.requests[2]["body"]["messages"]
    roles = [m["role"] for m in msgs]
    assert roles == (["user", "assistant", "user"] if api.anthropic
                     else ["system", "user", "assistant", "user"])
    assert msgs[-1]["content"] == "two"
    assert len(api.mock.requests) == 3


def test_truncated_reply_drops_tool_calls(api, tmp_path):
    api.reply("partial", [("t", "shell", {"command": "touch x"})],
              stop="max_tokens" if api.anthropic else "length")
    api.reply("next")
    p = api.run(tmp_path, "-y", stdin="a\nb\n")
    assert "max_tokens" in p.stderr and not (tmp_path / "x").exists()
    assert "tool_" not in json.dumps(api.mock.requests[1]["body"]["messages"])


# ---- provider selection ----

@pytest.mark.parametrize("provider,model", [
    ("anthropic", "claude-opus-5"), ("openrouter", "anthropic/claude-opus-5"), ("compat", "local")])
def test_default_models(mock, tmp_path, provider, model):
    api = Api(mock, provider)
    api.reply("hi")
    assert api.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[0]["body"]["model"] == model


def test_model_flag(mock, tmp_path):
    api = Api(mock, "openrouter")
    api.reply("hi")
    assert api.run(tmp_path, "-m", "openai/gpt-5.5", "-p", "x").returncode == 0
    assert mock.requests[0]["body"]["model"] == "openai/gpt-5.5"


def test_openrouter_is_default_provider(mock, tmp_path):
    api = Api(mock, "openrouter")
    api.reply("hi")
    p = subprocess.run([str(AGENT), "-p", "x"], cwd=tmp_path, env=api.env(), text=True,
                       capture_output=True, timeout=30)
    assert p.returncode == 0, p.stderr
    assert mock.requests[0]["path"] == "/api/v1/chat/completions"


def test_openai_requires_model(mock, tmp_path):
    p = subprocess.run([str(AGENT), "-P", "openai", "-p", "x"], cwd=tmp_path,
                       env=Api(mock, "openai").env(), capture_output=True, text=True, timeout=10)
    assert p.returncode == 2 and "needs -m" in p.stderr


def test_unknown_provider(tmp_path):
    p = subprocess.run([str(AGENT), "-P", "nope", "-p", "x"], cwd=tmp_path,
                       capture_output=True, text=True, timeout=10)
    assert p.returncode == 2 and "unknown provider nope" in p.stderr


@pytest.mark.parametrize("provider", ["anthropic", "openrouter", "openai"])
def test_missing_api_key(mock, tmp_path, provider):
    p = Api(mock, provider).run(tmp_path, "-p", "x", key=None)
    assert p.returncode == 1 and PROVIDERS[provider][0] + " is not set" in p.stderr
    assert not mock.requests


@pytest.mark.parametrize("key,header", [(None, None), ("sk-local", "Bearer sk-local")])
def test_compat_key_is_optional(mock, tmp_path, key, header):
    api = Api(mock, "compat")
    api.reply("hi")
    assert api.run(tmp_path, "-p", "x", key=key).returncode == 0
    assert mock.requests[0]["headers"].get("authorization") == header


# ---- wire-format specifics ----

def test_anthropic_request_shape(mock, tmp_path):
    api = Api(mock, "anthropic")
    api.reply("hi")
    api.run(tmp_path, "-p", "say hi")
    req = mock.requests[0]
    h, body = req["headers"], req["body"]
    assert h["x-api-key"] == "test-key" and "authorization" not in h
    assert h["anthropic-version"] == "2023-06-01"
    assert h["anthropic-beta"] == "server-side-fallback-2026-07-01"
    assert body["fallbacks"] == "default" and body["max_tokens"] == 16000
    assert body["messages"] == [{"role": "user", "content": "say hi"}]
    assert str(tmp_path.resolve()) in body["system"]


def test_openai_request_shape(mock, tmp_path):
    api = Api(mock, "openai")
    api.reply("hi")
    api.run(tmp_path, "-p", "say hi")
    req = mock.requests[0]
    h, body = req["headers"], req["body"]
    assert h["authorization"] == "Bearer test-key" and "x-api-key" not in h
    assert "anthropic-beta" not in h
    assert set(body) == {"model", "tools", "messages"}
    assert body["messages"][0]["role"] == "system"
    assert str(tmp_path.resolve()) in body["messages"][0]["content"]
    assert body["messages"][1] == {"role": "user", "content": "say hi"}
    t = body["tools"][0]
    assert t["type"] == "function" and t["function"]["parameters"]["required"] == ["path"]


def test_openai_assistant_tool_message_is_replayed(mock, tmp_path):
    api = Api(mock, "compat")
    api.reply(None, [("c1", "read", {"path": "f"}), ("c2", "shell", '{"command": ')])
    api.reply("ok")
    (tmp_path / "f").write_text("data")
    api.run(tmp_path, "-y", "-p", "go")
    msgs = mock.requests[1]["body"]["messages"]
    assert msgs[2]["role"] == "assistant" and msgs[2]["content"] is None
    assert [c["id"] for c in msgs[2]["tool_calls"]] == ["c1", "c2"]
    r = api.results(mock.requests[1])
    assert r["c1"] == ("data", False)
    assert r["c2"] == ("invalid JSON arguments for shell", True)


def test_openai_missing_choices_fails(mock, tmp_path):
    mock.replies.append((200, {"error": {"message": "upstream exploded"}}))
    p = Api(mock, "openrouter").run(tmp_path, "-p", "x")
    assert p.returncode == 1 and "upstream exploded" in p.stderr


def test_anthropic_thinking_replayed_and_refusal_rolled_back(mock, tmp_path):
    api = Api(mock, "anthropic")
    mock.replies.append((200, {"content": [
        {"type": "thinking", "thinking": "", "signature": "sig"},
        {"type": "text", "text": "first"}], "stop_reason": "end_turn"}))
    mock.replies.append((200, {"content": [], "stop_reason": "refusal",
                               "stop_details": {"type": "refusal", "explanation": "policy"}}))
    api.reply("second")
    p = api.run(tmp_path, stdin="one\nworse\ntwo\n")
    assert p.stdout == "first\nsecond\n" and "refused: policy" in p.stderr
    msgs = mock.requests[2]["body"]["messages"]
    assert [m["content"] if m["role"] == "user" else None for m in msgs] == ["one", None, "two"]
    assert msgs[1]["content"][0] == {"type": "thinking", "thinking": "", "signature": "sig"}


def test_openai_content_filter_rolled_back(mock, tmp_path):
    api = Api(mock, "openai")
    api.reply("", stop="content_filter")
    api.reply("fine")
    p = api.run(tmp_path, stdin="bad\ngood\n")
    assert "refused: content_filter" in p.stderr and p.stdout == "fine\n"
    assert [m["role"] for m in mock.requests[1]["body"]["messages"]] == ["system", "user"]


def test_openai_null_content_without_calls_becomes_empty(mock, tmp_path):
    api = Api(mock, "compat")
    api.reply(None)
    api.reply("ok")
    api.run(tmp_path, stdin="a\nb\n")
    assert mock.requests[1]["body"]["messages"][2] == {"role": "assistant", "content": ""}
