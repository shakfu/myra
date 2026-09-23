"""End-to-end tests: run ./myra against a scripted mock of /chat/completions."""

import fcntl
import json
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

AGENT = Path(os.environ.get("MYRA_BIN") or Path(__file__).resolve().parent.parent / "build/myra")

# provider -> (key env, base-url env, base-url suffix)
PROVIDERS = {
    "openrouter": ("OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "/api/v1"),
    "local": ("LOCAL_API_KEY", "LOCAL_BASE_URL", "/v1"),
}


def to_sse(reply):
    """A chat-completions reply as a stream: text in halves, arguments in thirds."""
    if "_sse" in reply:  # raw events, for malformed-stream tests
        return "".join(reply["_sse"]).encode()
    choice = reply["choices"][0]
    msg, deltas = choice["message"], [{"role": "assistant"}]
    for k, v in msg.items():
        if isinstance(v, str) and v and k != "role":
            deltas += [{k: v[:len(v) // 2]}, {k: v[len(v) // 2:]}]
        elif k == "reasoning_details":
            deltas.append({k: v})
    for i, tc in enumerate(msg.get("tool_calls") or []):
        fn = tc["function"]
        deltas.append({"tool_calls": [{"index": i, "id": tc["id"], "type": "function",
                                       "function": {"name": fn["name"], "arguments": ""}}]})
        a = fn["arguments"]
        for part in (a[:len(a) // 3], a[len(a) // 3:2 * len(a) // 3], a[2 * len(a) // 3:]):
            deltas.append({"tool_calls": [{"index": i, "function": {"arguments": part}}]})
    events = [": keep-alive comment\n\n"]
    events += ["data: " + json.dumps({"choices": [{"index": 0, "delta": d, "finish_reason": None}]})
               + "\n\n" for d in deltas]
    events.append("data: " + json.dumps({"choices": [{"index": 0, "delta": {},
                                                       "finish_reason": choice.get("finish_reason")}]})
                  + "\n\n")
    if "usage" in reply:
        events.append("data: " + json.dumps({"choices": [], "usage": reply["usage"]}) + "\n\n")
    events.append("data: [DONE]\n\n")
    return "".join(events).encode()


class Mock:
    """Serves queued (status, body) replies and records each request."""

    def __init__(self):
        self.replies, self.requests = [], []
        mock = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["content-length"]))
                self.answer("POST", json.loads(body))

            def do_GET(self):
                self.answer("GET", None)

            def answer(self, method, body):
                mock.requests.append({"method": method, "path": self.path,
                                      "headers": {k.lower(): v for k, v in self.headers.items()},
                                      "body": body})
                status, reply, *delay = mock.replies.pop(0)  # optional third item: seconds
                time.sleep(delay[0] if delay else 0)
                reply = dict(reply)
                wants_stream = (body or {}).get("stream") and status == 200
                plain = reply.pop("_plain", False) or (
                    "_sse" not in reply and not (wants_stream and reply.get("choices")))
                try:
                    if plain:
                        data = json.dumps(reply).encode()
                        self.send_response(status)
                        self.send_header("content-type", "application/json")
                        self.send_header("content-length", str(len(data)))
                        self.end_headers()
                        self.wfile.write(data)
                    else:  # server-sent events, in small writes so lines split mid-chunk
                        data = to_sse(reply)
                        self.send_response(200)
                        self.send_header("content-type", "text/event-stream")
                        self.end_headers()
                        for i in range(0, len(data), 50):
                            self.wfile.write(data[i:i + 50])
                            self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the agent gave up, e.g. after Ctrl-C

            def log_message(self, *a):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        # A short poll interval: shutdown() waits up to one interval (default 0.5 s).
        threading.Thread(target=self.server.serve_forever, args=(0.01,), daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"


class Api:
    """Runs the agent against the mock as one provider."""

    def __init__(self, mock, provider):
        self.mock, self.provider = mock, provider

    def env(self, cwd, key="test-key"):
        key_env, base_env, suffix = PROVIDERS[self.provider]
        env = {"PATH": "/usr/bin:/bin", base_env: self.mock.url + suffix,
               "XDG_STATE_HOME": str(cwd / ".state"), "MYRA_RETRY_DELAY_MS": "1"}
        if key is not None:
            env[key_env] = key
        return env

    def run(self, cwd, *args, stdin="", key="test-key", env=None, provider_flag=True):
        # New session: no controlling terminal, so /dev/tty confirmation cannot block.
        flag = ["-P", self.provider] if provider_flag else []
        return subprocess.run([str(AGENT), *flag, *args], cwd=cwd,
                              env={**self.env(cwd, key), **(env or {})}, input=stdin, text=True,
                              capture_output=True, timeout=60, start_new_session=True)

    def reply(self, text=None, calls=(), stop=None):
        """calls: (id, name, input dict or raw argument string)."""
        msg = {"role": "assistant", "content": text}
        if calls:
            msg["tool_calls"] = [{"id": i, "type": "function", "function": {
                "name": n, "arguments": a if isinstance(a, str) else json.dumps(a)}}
                for i, n, a in calls]
        stop = stop or ("tool_calls" if calls else "stop")
        self.mock.replies.append(
            (200, {"choices": [{"index": 0, "message": msg, "finish_reason": stop}]}))

    @staticmethod
    def results(req):
        """{tool call id: (content, is_error)} for the trailing tool messages in req."""
        out = {}
        for m in reversed(req["body"]["messages"]):
            if m["role"] != "tool":
                break
            out[m["tool_call_id"]] = (m["content"].removeprefix("error: "),
                                      m["content"].startswith("error: "))
        return out


@pytest.fixture
def mock():
    m = Mock()
    yield m
    m.server.shutdown()


@pytest.fixture(params=list(PROVIDERS))
def api(request, mock):
    return Api(mock, request.param)


# ---- behaviour shared by both providers ----

def test_request_shape(api, tmp_path):
    api.reply("hello")
    p = api.run(tmp_path, "-p", "say hi")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "hello\n"
    req = api.mock.requests[0]
    assert req["path"] == PROVIDERS[api.provider][2] + "/chat/completions"
    assert req["headers"]["authorization"] == "Bearer test-key"
    body = req["body"]
    if api.provider == "openrouter":
        assert body.pop("cache_control") == {"type": "ephemeral"}
    assert body.pop("stream") is True and body.pop("stream_options") == {"include_usage": True}
    assert set(body) == {"model", "tools", "messages"}  # nothing OpenRouter-specific to local
    assert body["messages"][0]["role"] == "system"
    assert str(tmp_path.resolve()) in body["messages"][0]["content"]
    assert body["messages"][1] == {"role": "user", "content": "say hi"}
    assert [t["type"] for t in body["tools"]] == ["function"] * 4
    fns = [t["function"] for t in body["tools"]]
    assert [f["name"] for f in fns] == ["read", "write", "edit", "shell"]
    assert fns[0]["parameters"]["required"] == ["path"]


def test_all_four_tools_in_one_turn(api, tmp_path):
    api.reply("working", [("t1", "write", {"path": "a.txt", "content": "one\ntwo\n"}),
                          ("t2", "edit", {"path": "a.txt", "old_string": "two", "new_string": "2"}),
                          ("t3", "read", {"path": "a.txt"}),
                          ("t4", "shell", {"command": "wc -l < a.txt; exit 3 # comment"})])
    api.reply("done")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "working\ndone\n"
    assert p.stderr == ("[tool] write a.txt\n[tool] edit a.txt\n[tool] read a.txt\n"
                        "[tool] shell wc -l < a.txt; exit 3 # comment\n")
    assert (tmp_path / "a.txt").read_text() == "one\n2\n"
    msgs = api.mock.requests[1]["body"]["messages"]
    assert msgs[2]["role"] == "assistant"
    assert [c["id"] for c in msgs[2]["tool_calls"]] == ["t1", "t2", "t3", "t4"]
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
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    content, err = api.results(api.mock.requests[1])["e"]
    assert err and msg in content
    assert (tmp_path / "b.txt").read_text() == "x x\n"


def test_write_and_edit_refuse_a_dangling_symlink(api, tmp_path):
    (tmp_path / "link").symlink_to("nowhere.txt")
    api.reply(calls=[("w", "write", {"path": "link", "content": "x"}),
                     ("e", "edit", {"path": "link", "old_string": "a", "new_string": "b"})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    r = api.results(api.mock.requests[1])
    for k in ("w", "e"):
        assert r[k][1] and r[k][0] == "link is a symlink to a missing file"
    assert (tmp_path / "link").is_symlink()          # the link survives
    assert not (tmp_path / "nowhere.txt").exists()   # and nothing was created


def test_edit_refuses_a_file_with_nul_bytes(api, tmp_path):
    (tmp_path / "bin").write_bytes(b"x\0x")  # two matches for "x", one on each side
    api.reply(calls=[("e", "edit", {"path": "bin", "old_string": "x", "new_string": "Y"})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    content, err = api.results(api.mock.requests[1])["e"]
    assert err and content == "bin is binary"
    assert (tmp_path / "bin").read_bytes() == b"x\0x"


def test_read_and_argument_errors(api, tmp_path):
    (tmp_path / "bin").write_bytes(b"a\0b")
    api.reply(calls=[("r1", "read", {"path": "bin"}), ("r2", "read", {"path": "nope"}),
                     ("r3", "bogus", {}), ("r4", "shell", '{"command": ')])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    r = api.results(api.mock.requests[1])
    assert r["r1"][1] and "binary" in r["r1"][0]
    assert r["r2"][1] and "cannot read" in r["r2"][0]
    assert r["r3"][1] and "unknown tool" in r["r3"][0]
    assert r["r4"] == ("invalid JSON arguments for shell", True)


@pytest.mark.parametrize("env,cap", [({}, None), ({"MYRA_MAX_OUTPUT": "4096"}, 4096)])
def test_shell_output_keeps_start_and_end(api, tmp_path, env, cap):
    cap = cap or {"openrouter": 102400, "local": 16384}[api.provider]
    api.reply(calls=[("s", "shell", {"command": "seq 1 100000"})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go", env=env)
    out = api.results(api.mock.requests[1])["s"][0]
    head, sep, rest = out.partition("\n[... ")
    assert sep and len(head) == cap // 5 and head.startswith("1\n2\n3\n")
    omitted, _, tail = rest.partition(" bytes omitted ...]\n")
    assert tail.endswith("99999\n100000\n[exit 0]")
    assert len(head) + int(omitted) + len(tail) - len("[exit 0]") == 588895  # seq's size


def test_mutating_tools_denied_without_tty(api, tmp_path):
    (tmp_path / "e.txt").write_text("readable")
    api.reply(calls=[("w", "write", {"path": "c.txt", "content": "x"}),
                     ("s", "shell", {"command": "touch d.txt"}),
                     ("r", "read", {"path": "e.txt"})])
    api.reply("ok")
    p = api.run(tmp_path, "--permissions", "ask", "-p", "go")
    assert p.returncode == 0, p.stderr
    r = api.results(api.mock.requests[1])
    assert r["w"] == ("denied: no terminal to confirm; --permissions auto skips asking", True)
    assert r["s"] == ("denied: no terminal to confirm; --permissions auto skips asking", True)
    assert r["r"] == ("readable", False)  # read needs no confirmation
    assert not (tmp_path / "c.txt").exists() and not (tmp_path / "d.txt").exists()


def test_default_retry_delay_is_one_second(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((503, {"error": {}}))
    api.reply("ok")
    t = time.monotonic()
    p = api.run(tmp_path, "-p", "go", env={"MYRA_RETRY_DELAY_MS": ""})
    assert p.returncode == 0 and 0.9 < time.monotonic() - t < 3


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


def test_missing_choices_fails(api, tmp_path):
    api.mock.replies.append((200, {"error": {"message": "upstream exploded"}}))
    p = api.run(tmp_path, "-p", "x")
    assert p.returncode == 1 and "upstream exploded" in p.stderr


def test_repl_keeps_history_and_rolls_back_failures(api, tmp_path):
    api.reply("first")
    api.mock.replies.append((400, {"error": {"message": "nope"}}))
    api.reply("", stop="content_filter")
    api.reply("second")
    p = api.run(tmp_path, stdin="one\n\nbad\nworse\ntwo\n/exit\nnever\n")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "first\nsecond\n"
    assert "refused: content_filter" in p.stderr
    msgs = api.mock.requests[3]["body"]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user", "assistant", "user"]
    assert msgs[-1]["content"] == "two"
    assert len(api.mock.requests) == 4


def test_truncated_reply_drops_tool_calls(api, tmp_path):
    api.reply("partial", [("t", "shell", {"command": "touch x"})], stop="length")
    api.reply("next")
    p = api.run(tmp_path, stdin="a\nb\n")
    assert "length limit" in p.stderr and not (tmp_path / "x").exists()
    msgs = api.mock.requests[1]["body"]["messages"]
    assert msgs[2] == {"role": "assistant", "content": "partial"}


def test_null_content_without_calls_becomes_empty(api, tmp_path):
    api.reply(None)
    api.reply("ok")
    api.run(tmp_path, stdin="a\nb\n")
    assert api.mock.requests[1]["body"]["messages"][2] == {"role": "assistant", "content": ""}


# ---- provider selection ----

@pytest.mark.parametrize("provider,model", [
    ("openrouter", "anthropic/claude-opus-5"), ("local", "local")])
def test_default_models(mock, tmp_path, provider, model):
    api = Api(mock, provider)
    api.reply("hi")
    assert api.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[0]["body"]["model"] == model


def test_model_flag(api, tmp_path):
    api.reply("hi")
    assert api.run(tmp_path, "-m", "openai/gpt-5.5", "-p", "x").returncode == 0
    assert api.mock.requests[0]["body"]["model"] == "openai/gpt-5.5"


def test_openrouter_is_default_when_key_set(mock, tmp_path):
    api = Api(mock, "openrouter")
    api.reply("hi")
    p = api.run(tmp_path, "-p", "x", provider_flag=False)
    assert p.returncode == 0, p.stderr
    assert mock.requests[0]["path"] == "/api/v1/chat/completions"


@pytest.mark.parametrize("key", [None, ""])
def test_no_key_and_no_provider_is_an_error(mock, tmp_path, key):
    env = {} if key is None else {"OPENROUTER_API_KEY": key}
    p = Api(mock, "local").run(tmp_path, "-p", "x", provider_flag=False, key=None, env=env)
    assert p.returncode == 1
    assert "set OPENROUTER_API_KEY or pass -P local" in p.stderr
    assert not mock.requests  # never falls back to local


@pytest.mark.parametrize("name", ["local", "openrouter"])
@pytest.mark.parametrize("flags", [[], ["-P", "local"]])
def test_provider_name_as_prompt_hints_at_P(mock, tmp_path, name, flags):
    remember(tmp_path, "local")  # would otherwise run: the prompt must still be refused
    p = Api(mock, "local").run(tmp_path, *flags, "-p", name, provider_flag=False)
    assert p.returncode == 2
    assert p.stderr == f"error: -p takes a prompt; did you mean -P {name}?\n"
    assert not mock.requests


@pytest.mark.parametrize("name", ["nope", "anthropic", "openai", "compat"])
def test_unknown_provider(tmp_path, name):
    p = subprocess.run([str(AGENT), "-P", name, "-p", "x"], cwd=tmp_path,
                       capture_output=True, text=True, timeout=10)
    assert p.returncode == 2 and f"unknown provider {name}" in p.stderr


def test_openrouter_requires_key(mock, tmp_path):
    p = Api(mock, "openrouter").run(tmp_path, "-p", "x", key=None)
    assert p.returncode == 1 and "OPENROUTER_API_KEY is not set" in p.stderr
    assert not mock.requests


@pytest.mark.parametrize("key,header", [(None, None), ("sk-local", "Bearer sk-local")])
def test_local_key_is_optional(mock, tmp_path, key, header):
    api = Api(mock, "local")
    api.reply("hi")
    assert api.run(tmp_path, "-p", "x", key=key).returncode == 0
    assert mock.requests[0]["headers"].get("authorization") == header


# ---- remembered model ----

def test_model_is_remembered_per_provider(mock, tmp_path):
    api = Api(mock, "openrouter")
    api.reply("a")
    assert api.run(tmp_path, "-m", "openai/gpt-5.5", "-p", "x").returncode == 0
    assert (tmp_path / ".state/myra/openrouter.model").read_text() == "openai/gpt-5.5\n"
    api.reply("b")
    assert api.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[1]["body"]["model"] == "openai/gpt-5.5"

    local = Api(mock, "local")  # other provider keeps its own default
    local.reply("c")
    assert local.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[2]["body"]["model"] == "local"

    api.reply("d")  # a new -m replaces the remembered one
    assert api.run(tmp_path, "-m", "anthropic/claude-opus-4.8", "-p", "x").returncode == 0
    assert (tmp_path / ".state/myra/openrouter.model").read_text() == "anthropic/claude-opus-4.8\n"


def test_rejected_model_is_not_remembered(mock, tmp_path):
    api = Api(mock, "openrouter")
    mock.replies.append((400, {"error": {"message": "no such model"}}))
    assert api.run(tmp_path, "-m", "typo", "-p", "x").returncode == 1
    assert not (tmp_path / ".state/myra/openrouter.model").exists()
    api.reply("hi")
    api.run(tmp_path, "-p", "x")
    assert mock.requests[1]["body"]["model"] == "anthropic/claude-opus-5"


def test_blank_state_file_uses_default(mock, tmp_path):
    (tmp_path / ".state/myra").mkdir(parents=True)
    (tmp_path / ".state/myra/local.model").write_text(" \n")
    api = Api(mock, "local")
    api.reply("hi")
    api.run(tmp_path, "-p", "x")
    assert mock.requests[0]["body"]["model"] == "local"


def test_state_falls_back_to_home(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    p = api.run(tmp_path, "-m", "qwen", "-p", "x", env={"XDG_STATE_HOME": "", "HOME": str(tmp_path)})
    assert p.returncode == 0, p.stderr
    assert (tmp_path / ".local/state/myra/local.model").read_text() == "qwen\n"


def test_unwritable_state_warns_but_runs(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    p = api.run(tmp_path, "-m", "qwen", "-p", "x", env={"XDG_STATE_HOME": "/dev/null/x"})
    assert p.returncode == 0 and p.stdout == "hi\n"
    assert "cannot save /dev/null/x/myra/" in p.stderr


def test_repl_shows_provider_and_model(mock, tmp_path):
    api = Api(mock, "openrouter")
    p = api.run(tmp_path, stdin="/model\n")
    assert p.returncode == 0
    # Piped input: banner and /model output only, no "> " prompts, no color.
    assert p.stderr == "myra 0.1.2\n" + "openrouter anthropic/claude-opus-5\n" * 2


# ---- provider selection: -P, else remembered if usable, else first cloud key, else error ----

def both_keys(tmp_path, mock):
    """Env where either provider could run: OpenRouter key set, both base URLs at the mock."""
    return {**Api(mock, "local").env(tmp_path), **Api(mock, "openrouter").env(tmp_path)}


def run_plain(tmp_path, env, *args):
    return subprocess.run([str(AGENT), *args], cwd=tmp_path, env=env, text=True,
                          capture_output=True, timeout=30, start_new_session=True)


def remember(tmp_path, provider):
    (tmp_path / ".state/myra").mkdir(parents=True, exist_ok=True)
    (tmp_path / ".state/myra/provider").write_text(provider + "\n")


def test_provider_is_remembered(mock, tmp_path):
    env = both_keys(tmp_path, mock)
    Api(mock, "local").reply("a")
    assert run_plain(tmp_path, env, "-P", "local", "-p", "x").returncode == 0
    assert (tmp_path / ".state/myra/provider").read_text() == "local\n"
    Api(mock, "local").reply("b")  # key is set, but the remembered provider wins
    assert run_plain(tmp_path, env, "-p", "x").returncode == 0
    Api(mock, "openrouter").reply("c")  # -P switches and is remembered
    assert run_plain(tmp_path, env, "-P", "openrouter", "-p", "x").returncode == 0
    Api(mock, "openrouter").reply("d")
    assert run_plain(tmp_path, env, "-p", "x").returncode == 0
    assert [r["path"] for r in mock.requests] == (["/v1/chat/completions"] * 2
                                                  + ["/api/v1/chat/completions"] * 2)


def test_provider_and_model_remembered_together(mock, tmp_path):
    env = both_keys(tmp_path, mock)
    Api(mock, "local").reply("a")
    assert run_plain(tmp_path, env, "-P", "local", "-m", "qwen", "-p", "x").returncode == 0
    Api(mock, "local").reply("b")
    assert run_plain(tmp_path, env, "-p", "x").returncode == 0
    assert mock.requests[1]["path"] == "/v1/chat/completions"
    assert mock.requests[1]["body"]["model"] == "qwen"


def test_default_provider_is_not_saved(mock, tmp_path):
    Api(mock, "openrouter").reply("hi")  # picked by the key rule, not by -P
    assert run_plain(tmp_path, both_keys(tmp_path, mock), "-p", "x").returncode == 0
    assert not (tmp_path / ".state/myra/provider").exists()


def test_rejected_provider_is_not_remembered(mock, tmp_path):
    mock.replies.append((401, {"error": {"message": "bad key"}}))
    assert run_plain(tmp_path, both_keys(tmp_path, mock), "-P", "local", "-p", "x").returncode == 1
    assert not (tmp_path / ".state/myra/provider").exists()


def test_unknown_provider_is_not_remembered(mock, tmp_path):
    assert run_plain(tmp_path, both_keys(tmp_path, mock), "-P", "nope", "-p", "x").returncode == 2
    assert not (tmp_path / ".state/myra").exists()


def test_remembered_local_needs_no_key(mock, tmp_path):
    remember(tmp_path, "local")
    api = Api(mock, "local")
    api.reply("hi")
    p = api.run(tmp_path, "-p", "x", provider_flag=False, key=None)
    assert p.returncode == 0, p.stderr
    assert mock.requests[0]["path"] == "/v1/chat/completions"


def test_remembered_cloud_without_key_is_skipped(mock, tmp_path):
    remember(tmp_path, "openrouter")
    p = Api(mock, "local").run(tmp_path, "-p", "x", provider_flag=False, key=None)
    assert p.returncode == 1 and "set OPENROUTER_API_KEY or pass -P local" in p.stderr
    assert not mock.requests


def test_invalid_remembered_provider_falls_back(mock, tmp_path):
    remember(tmp_path, "anthropic")
    Api(mock, "openrouter").reply("hi")
    p = run_plain(tmp_path, both_keys(tmp_path, mock), "-p", "x")
    assert p.returncode == 0, p.stderr
    assert mock.requests[0]["path"] == "/api/v1/chat/completions"


def test_explicit_local_ignores_cloud_key(mock, tmp_path):
    Api(mock, "local").reply("hi")
    env = {**Api(mock, "local").env(tmp_path, key=None), "OPENROUTER_API_KEY": "sk-or"}
    p = run_plain(tmp_path, env, "-P", "local", "-p", "x")
    assert p.returncode == 0, p.stderr
    assert mock.requests[0]["path"] == "/v1/chat/completions"
    assert "authorization" not in mock.requests[0]["headers"]  # OpenRouter key not sent


# ---- /model ----

def test_model_command_switches_and_keeps_history(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("a")
    api.reply("b")
    p = api.run(tmp_path, stdin="one\n/model\n/model   qwen \ntwo\n")
    assert p.returncode == 0, p.stderr
    assert p.stderr.count("local local\n") == 2  # banner, then bare /model
    assert "local qwen \n" not in p.stderr and "local qwen\n" in p.stderr
    assert [r["body"]["model"] for r in mock.requests] == ["local", "qwen"]
    msgs = mock.requests[1]["body"]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user", "assistant", "user"]
    assert "/model" not in json.dumps(msgs)
    assert (tmp_path / ".state/myra/local.model").read_text() == "qwen\n"


def test_model_command_not_saved_until_accepted(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((400, {"error": {"message": "no such model"}}))
    p = api.run(tmp_path, stdin="/model typo\nhi\n")
    assert "no such model" in p.stderr
    assert not (tmp_path / ".state/myra/local.model").exists()


def test_model_prefix_is_not_a_command(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("ok")
    api.run(tmp_path, stdin="/modelling question\n")
    assert mock.requests[0]["method"] == "POST"
    assert mock.requests[0]["body"]["messages"][1]["content"] == "/modelling question"


# ---- /models ----

MODELS = {"object": "list", "data": [{"id": "anthropic/claude-opus-5"}, {"id": "openai/gpt-5.5"},
                                     {"id": "anthropic/claude-sonnet-5"}, {"name": "no id"},
                                     {"id": "/models/Qwen3-4B-Q8_0.gguf"}]}


def test_models_lists_and_stars_current(api, tmp_path):
    api.mock.replies.append((200, MODELS))
    api.run(tmp_path, "-m", "openai/gpt-5.5", stdin="/models\n")
    req = api.mock.requests[0]
    assert req["method"] == "GET"
    assert req["path"] == PROVIDERS[api.provider][2] + "/models"
    assert req["headers"]["authorization"] == "Bearer test-key"


def test_models_output_and_filter(mock, tmp_path):
    api = Api(mock, "openrouter")
    mock.replies += [(200, MODELS), (200, MODELS)]
    p = api.run(tmp_path, stdin="/models\n/models   claude \n")
    assert p.returncode == 0, p.stderr
    assert p.stdout == ("* anthropic/claude-opus-5\n  openai/gpt-5.5\n  anthropic/claude-sonnet-5\n"
                        "  /models/Qwen3-4B-Q8_0.gguf\n"
                        "* anthropic/claude-opus-5\n  anthropic/claude-sonnet-5\n")


@pytest.mark.parametrize("filter_", ["qwen", "QWEN3", "q8_0.GGUF"])
def test_models_filter_ignores_case(mock, tmp_path, filter_):
    mock.replies.append((200, MODELS))
    p = Api(mock, "local").run(tmp_path, stdin=f"/models {filter_}\n")
    assert p.stdout == "  /models/Qwen3-4B-Q8_0.gguf\n"


def test_models_does_not_touch_history(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((200, MODELS))
    api.reply("hi")
    api.run(tmp_path, stdin="/models\nhello\n")
    assert [m["role"] for m in mock.requests[1]["body"]["messages"]] == ["system", "user"]


@pytest.mark.parametrize("reply,msg", [
    ((401, {"error": {"message": "bad key"}}), "bad key"),
    ((200, {"error": {"message": "odd"}}), "no model list"),
])
def test_models_errors_keep_repl_running(mock, tmp_path, reply, msg):
    api = Api(mock, "local")
    mock.replies.append(reply)
    api.reply("still here")
    p = api.run(tmp_path, stdin="/models\nhello\n")
    assert msg in p.stderr and p.stdout == "still here\n"


def test_models_retries_server_errors(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies += [(503, {"error": {}}), (200, MODELS)]
    p = api.run(tmp_path, stdin="/models gpt\n")
    assert "retrying" in p.stderr and p.stdout == "  openai/gpt-5.5\n"


# ---- connection refused ----

def closed_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.mark.parametrize("stdin", [None, "/models\nhi\n"])
def test_refused_connection_fails_fast(tmp_path, stdin):
    url = f"http://127.0.0.1:{closed_port()}/v1"
    env = {"PATH": "/usr/bin:/bin", "LOCAL_BASE_URL": url, "XDG_STATE_HOME": str(tmp_path / ".state")}
    args = ["-p", "x"] if stdin is None else []
    t = time.monotonic()
    p = subprocess.run([str(AGENT), "-P", "local", *args], cwd=tmp_path, env=env, input=stdin or "",
                       text=True, capture_output=True, timeout=30, start_new_session=True)
    assert time.monotonic() - t < 3  # no 1+2+4+8 s back-off
    assert "retrying" not in p.stderr
    msg = f"cannot connect to {url}/%s; is the server running?"
    if stdin is None:
        assert p.returncode == 1 and msg % "chat/completions" in p.stderr
    else:  # REPL survives both /models and a prompt
        assert p.returncode == 0
        assert msg % "models" in p.stderr and msg % "chat/completions" in p.stderr
    assert not (tmp_path / ".state/myra/provider").exists()


# ---- Ctrl-C (SIGINT) ----

def start(api, cwd, *args):
    """Start the agent without waiting; stderr's first line is read by the caller if needed."""
    return subprocess.Popen([str(AGENT), "-P", api.provider, *args], cwd=cwd, env=api.env(cwd),
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)


def wait_for(cond, timeout=10):
    end = time.monotonic() + timeout
    while not cond():
        assert time.monotonic() < end, "timed out waiting"
        time.sleep(0.05)


def pid_gone(path):
    pid = int(path.read_text())
    for _ in range(40):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.05)
    return False


def test_sigint_during_shell_kills_it_and_exits_130(mock, tmp_path):
    api = Api(mock, "local")
    api.reply(calls=[("s", "shell", {"command": "sleep 30 & echo $! > pid; wait"}),
                     ("t", "shell", {"command": "touch never"})])
    p = start(api, tmp_path, "-p", "go")
    wait_for(lambda: (tmp_path / "pid").exists() and (tmp_path / "pid").read_text().strip())
    t = time.monotonic()
    p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=10)
    assert time.monotonic() - t < 4
    assert p.returncode == 130, err
    assert "interrupted" in err
    assert pid_gone(tmp_path / "pid")  # the command's child was killed too
    assert not (tmp_path / "never").exists()  # the second call was skipped
    assert len(mock.requests) == 1  # no follow-up request after the interrupt


def test_second_sigint_does_not_stop_the_kill(mock, tmp_path):
    api = Api(mock, "local")  # SIGTERM is ignored, so only the SIGKILL 2 s later ends it
    api.reply(calls=[("s", "shell", {"command": "trap '' TERM; sleep 30 & echo $! > pid; wait"})])
    p = start(api, tmp_path, "-p", "go")
    wait_for(lambda: (tmp_path / "pid").exists() and (tmp_path / "pid").read_text().strip())
    p.send_signal(signal.SIGINT)
    time.sleep(0.5)  # inside the grace period
    p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=10)
    assert p.returncode == 130, err
    assert pid_gone(tmp_path / "pid")


def test_sigint_during_request_exits_130(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((200, {"choices": []}, 20))  # the server stalls
    p = start(api, tmp_path, "-p", "go")
    wait_for(lambda: mock.requests)
    t = time.monotonic()
    p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=10)
    assert time.monotonic() - t < 3  # curl checks roughly once a second
    assert p.returncode == 130 and "interrupted" in err
    assert "retrying" not in err


def test_repl_survives_sigint_at_prompt_and_mid_turn(mock, tmp_path):
    api = Api(mock, "local")
    api.reply(calls=[("s", "shell", {"command": "echo $$ > pid; sleep 30"})])
    api.reply("after")
    p = start(api, tmp_path)
    assert p.stderr.readline() == "myra 0.1.2\n"
    assert p.stderr.readline() == "local local\n"  # handler is installed by now
    p.send_signal(signal.SIGINT)  # at the prompt: the line is dropped, the REPL stays
    time.sleep(0.3)
    p.stdin.write("first\n")
    p.stdin.flush()
    wait_for(lambda: (tmp_path / "pid").exists() and (tmp_path / "pid").read_text().strip())
    p.send_signal(signal.SIGINT)  # mid-turn: the command is killed, the turn ends
    wait_for(lambda: pid_gone(tmp_path / "pid"))
    out, err = p.communicate("second\n", timeout=10)
    assert p.returncode == 0, err
    assert out == "after\n"
    msgs = mock.requests[1]["body"]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user", "assistant", "tool", "user"]
    assert msgs[3]["content"] == "error: [interrupted by user; killed]"
    assert msgs[4]["content"] == "second"


# ---- /clear ----

def test_clear_starts_a_new_conversation(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("a")
    api.reply("b")
    p = api.run(tmp_path, "-m", "qwen", stdin="one\n/clear\ntwo\n/model\n")
    assert p.returncode == 0, p.stderr
    assert "history cleared\n" in p.stderr
    assert "local qwen\n" in p.stderr  # model and provider are kept
    msgs = mock.requests[1]["body"]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user"]
    assert msgs[0] == mock.requests[0]["body"]["messages"][0]  # same system prompt
    assert msgs[1]["content"] == "two" and mock.requests[1]["body"]["model"] == "qwen"


# ---- UTF-8 ----

def test_invalid_utf8_tool_output_reaches_server_as_valid_json(api, tmp_path):
    (tmp_path / "l1.txt").write_bytes(b"caf\xe9\n")
    api.reply(calls=[("s", "shell", {"command": "printf '\\377ok\\000!'"}),
                     ("r", "read", {"path": "l1.txt"})])
    api.reply("ok")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    r = api.results(api.mock.requests[1])  # the mock json-decoded this body
    assert r["s"] == ("\ufffdok\ufffd!\n[exit 0]", False)
    assert r["r"] == ("caf\ufffd\n", False)


# ---- context overflow ----

CONTEXT_ERRORS = [  # llama-server, OpenAI-style, Anthropic via OpenRouter
    {"error": {"code": 400, "type": "exceed_context_size_error",
               "message": "the request exceeds the available context size, try increasing it"}},
    {"error": {"message": "This model's maximum context length is 8192 tokens."}},
    {"error": {"message": "prompt is too long: 250000 tokens > 200000 maximum"}},
]


@pytest.mark.parametrize("body", CONTEXT_ERRORS)
def test_context_full_drops_old_tool_output_and_retries(mock, tmp_path, body):
    (tmp_path / "f").write_text("data")
    api = Api(mock, "local")
    api.reply(calls=[("c1", "read", {"path": "f"})])
    api.reply("ok")
    mock.replies.append((400, body))
    api.reply("fine")
    p = api.run(tmp_path, stdin="one\ntwo\n")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "ok\nfine\n"
    assert "context full: dropped 1 old tool output, retrying" in p.stderr
    msgs = mock.requests[3]["body"]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user", "assistant", "tool", "assistant", "user"]
    assert msgs[3]["content"] == "[output dropped to fit the context]"
    assert msgs[3]["tool_call_id"] == "c1"  # the call/result pairing is kept


def test_context_full_keeps_newest_batch_then_hints(mock, tmp_path):
    (tmp_path / "f").write_text("data")
    api = Api(mock, "local")
    api.reply(calls=[("c1", "read", {"path": "f"})])
    mock.replies.append((400, CONTEXT_ERRORS[0]))
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 1
    assert "/clear starts a new one" in p.stderr and "dropped" not in p.stderr
    assert len(mock.requests) == 2  # no pointless retry
    assert api.results(mock.requests[1])["c1"] == ("data", False)


def test_other_400_is_not_a_context_error(mock, tmp_path):
    mock.replies.append((400, {"error": {"message": "invalid model"}}))
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 1 and "invalid model" in p.stderr
    assert "/clear" not in p.stderr and len(mock.requests) == 1


# ---- usage and /help ----

def test_usage_is_summed_per_turn(mock, tmp_path):
    api = Api(mock, "openrouter")
    api.reply(calls=[("c", "read", {"path": "nope"})])
    mock.replies[-1][1]["usage"] = {"prompt_tokens": 1000, "completion_tokens": 20,
                                    "prompt_tokens_details": {"cached_tokens": 0}}
    api.reply("done")
    mock.replies[-1][1]["usage"] = {"prompt_tokens": 1100, "completion_tokens": 5,
                                    "prompt_tokens_details": {"cached_tokens": 900}}
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    assert p.stderr.endswith("[usage] 2 requests: 2100 in (900 cached), 25 out\n")


def test_partial_usage_counts_missing_fields_as_zero(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    mock.replies[-1][1]["usage"] = {"prompt_tokens": 7}
    p = api.run(tmp_path, "-p", "go")
    assert p.stderr == "[usage] 1 request: 7 in (0 cached), 0 out\n"


def test_no_usage_no_line(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    assert "[usage]" not in api.run(tmp_path, "-p", "go").stderr


def test_help_lists_commands(mock, tmp_path):
    p = Api(mock, "local").run(tmp_path, stdin="/help\n")
    for cmd in ["/model [id]", "/models [filter]", "/clear", "/help", "/exit"]:
        assert cmd in p.stderr
    assert not mock.requests


# ---- streaming ----

def sse(*chunks, done=True, nl="\n"):
    """Raw events: each chunk is a delta dict, or a full event dict with a "choices" key."""
    out = []
    for c in chunks:
        ev = c if "choices" in c or "error" in c else {"choices": [{"index": 0, "delta": c}]}
        out.append("data: " + json.dumps(ev) + nl + nl)
    if done:
        out.append("data: [DONE]" + nl + nl)
    return {"_sse": out}


def finish(reason):
    return {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]}


def test_reasoning_content_is_replayed(mock, tmp_path):
    api = Api(mock, "local")
    api.reply(calls=[("c", "read", {"path": "nope"})])
    mock.replies[-1][1]["choices"][0]["message"]["reasoning_content"] = "think step by step"
    api.reply("done")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    msg = mock.requests[1]["body"]["messages"][2]
    assert msg["reasoning_content"] == "think step by step" and msg["content"] is None
    assert p.stdout == "done\n"  # reasoning is not printed


def test_reasoning_details_are_merged_by_index(mock, tmp_path):
    mock.replies.append((200, sse(
        {"reasoning_details": [{"type": "reasoning.text", "text": "ab", "index": 0}]},
        {"reasoning_details": [{"type": "reasoning.text", "text": "cd", "index": 0,
                                "signature": "sig"}]},
        {"reasoning_details": [{"type": "reasoning.encrypted", "data": "xyz", "index": 1}]},
        {"content": "hi"}, finish("stop"))))
    Api(mock, "local").reply("again")
    p = Api(mock, "local").run(tmp_path, stdin="one\ntwo\n")
    assert p.stdout == "hi\nagain\n", p.stderr
    assert mock.requests[1]["body"]["messages"][2]["reasoning_details"] == [
        {"type": "reasoning.text", "text": "abcd", "index": 0, "signature": "sig"},
        {"type": "reasoning.encrypted", "data": "xyz", "index": 1}]


def test_crlf_events_and_missing_done(mock, tmp_path):
    mock.replies.append((200, sse({"content": "a"}, {"content": "b"}, finish("stop"),
                                  done=False, nl="\r\n")))
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "ab\n", p.stderr


def test_server_that_ignores_stream_still_works(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("plain reply")
    mock.replies[-1][1]["_plain"] = True
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "plain reply\n"  # printed once


def test_midstream_error_before_text_is_retried(mock, tmp_path):
    mock.replies.append((200, sse({"role": "assistant"},
                                  {"error": {"message": "upstream overloaded"}})))
    Api(mock, "local").reply("recovered")
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "recovered\n"
    assert "upstream overloaded" in p.stderr and "retrying" in p.stderr


def test_midstream_error_after_text_is_not_retried(mock, tmp_path):
    mock.replies.append((200, sse({"content": "partial answ"},
                                  {"error": {"message": "upstream died"}})))
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 1
    assert p.stdout == "partial answ\n"  # the line is ended, and nothing is printed twice
    assert "upstream died" in p.stderr and "retrying" not in p.stderr
    assert len(mock.requests) == 1


def test_midstream_context_error_is_recognised(mock, tmp_path):
    mock.replies.append((200, sse({"error": {"message": "prompt is too long: 300000 tokens"}})))
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 1 and "/clear starts a new one" in p.stderr


# ---- line editing (libedit), driven through a pseudo-terminal ----

class Tty:
    """The agent on a pty, as if typed into a terminal. expect() matches in order."""

    def __init__(self, api, cwd, *args, env=None):
        # A terminal's usual locale; Linux CI images ship C.UTF-8 but not en_US.UTF-8.
        env = {**api.env(cwd), "LANG": "en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8",
               **(env or {})}
        self.pid, self.fd = pty.fork()  # the child gets the pty as its controlling terminal
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(str(AGENT), [str(AGENT), "-P", api.provider, *args], env)
        # A pty starts at 0x0. linenoise reads the size to lay out the line, and with
        # no size it falls back to asking the terminal with ESC[6n and waits for a reply.
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.out, self.pos = b"", 0

    def expect(self, pattern, timeout=10):
        end = time.monotonic() + timeout
        while (i := self.out.find(pattern, self.pos)) < 0:
            assert time.monotonic() < end, f"waiting for {pattern!r} after {self.out[self.pos:]!r}"
            if select.select([self.fd], [], [], 0.05)[0]:
                try:
                    self.out += os.read(self.fd, 4096)
                except OSError:
                    pass
        self.pos = i + len(pattern)

    def type(self, data):
        os.write(self.fd, data)

    def wait(self, timeout=10):
        end = time.monotonic() + timeout
        while not (r := os.waitpid(self.pid, os.WNOHANG))[0]:
            if time.monotonic() > end:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
                raise AssertionError(f"agent did not exit: {self.out!r}")
            if select.select([self.fd], [], [], 0.05)[0]:
                try:
                    self.out += os.read(self.fd, 4096)
                except OSError:
                    pass
        os.close(self.fd)
        return os.waitstatus_to_exitcode(r[1])


@pytest.mark.filterwarnings("ignore::DeprecationWarning")  # fork() with the mock's thread
def test_terminal_line_editing_history_and_ctrl_c(mock, tmp_path):
    api = Api(mock, "local")
    for r in ("r1", "r2", "r3"):
        api.reply(r)
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"ello\x01h\r")  # Ctrl-A jumps to the start: linenoise is editing the line
    tty.expect(b"r1")
    tty.expect(b"> ")
    tty.type(b"\x1b[A\r")  # up arrow recalls "hello"
    tty.expect(b"r2")
    tty.expect(b"> ")
    tty.type(b"abc")
    tty.expect(b"abc")
    tty.type(b"\x03")  # Ctrl-C drops the line; the first press is enough
    tty.expect(b"\r\n")  # linenoise brackets each line with \x1b[?2004h/l, so not contiguous
    tty.expect(b"> ")
    tty.type("café\r".encode())  # multi-byte input survives the custom getc
    tty.expect(b"r3")
    tty.expect(b"> ")
    tty.type(b"/exit\r")
    assert tty.wait() == 0
    assert [r["body"]["messages"][-1]["content"] for r in mock.requests] == ["hello", "hello", "café"]
    hist = tmp_path / ".state/myra/history"
    assert hist.stat().st_mode & 0o777 == 0o600 and "hello" in hist.read_text(errors="replace")


@pytest.mark.filterwarnings("ignore::DeprecationWarning")
def test_terminal_history_persists_across_runs(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("first")
    api.reply("second")
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"remember me\r")
    tty.expect(b"first")
    tty.expect(b"> ", 2)
    tty.type(b"\x04")  # Ctrl-D on an empty line quits
    assert tty.wait() == 0
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"\x1b[A\r")
    tty.expect(b"second")
    tty.expect(b"> ", 2)
    tty.type(b"\x04")
    assert tty.wait() == 0
    assert mock.requests[1]["body"]["messages"][-1]["content"] == "remember me"


# ---- tab completion ----

@pytest.mark.filterwarnings("ignore::DeprecationWarning")
def test_tab_completes_a_repl_command(mock, tmp_path):
    api = Api(mock, "local")
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"/mod\t\r")  # /model and /models both match; the first is offered
    tty.expect(b"local local")  # /model with no argument reports the current model
    tty.expect(b"> ", 2)  # linenoise discards input typed before it re-enters raw mode
    tty.type(b"\x04")
    assert tty.wait() == 0


@pytest.mark.filterwarnings("ignore::DeprecationWarning")
def test_tab_completes_a_model_id(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((200, MODELS))
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"/model anthropic/claude-o\t\r")
    # the result line, not the echoed completion: the redraw puts a "> " after that too
    tty.expect(b"local anthropic/claude-opus-5")
    tty.expect(b"> ", 2)
    tty.type(b"\x04")
    assert tty.wait() == 0
    assert mock.requests[0]["method"] == "GET"  # the ids come from one /models request


@pytest.mark.filterwarnings("ignore::DeprecationWarning")
def test_tab_completes_a_path(mock, tmp_path):
    (tmp_path / "notes.txt").write_text("x")
    api = Api(mock, "local")
    api.reply("ok")
    tty = Tty(api, tmp_path)
    tty.expect(b"> ")
    tty.type(b"read not\t\r")
    tty.expect(b"ok")
    tty.expect(b"> ", 2)
    tty.type(b"\x04")
    assert tty.wait() == 0
    assert mock.requests[0]["body"]["messages"][-1]["content"] == "read notes.txt"


# ---- --permissions: auto (the default) covers only the working directory ----

OUTSIDE = "denied: outside the working directory; --permissions all allows it"


def test_auto_does_not_cover_writes_outside_cwd(mock, tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (tmp_path / "out.txt").write_text("keep")
    (work / "link").symlink_to(tmp_path)  # a symlink out of the working directory
    api = Api(mock, "local")
    api.reply(calls=[("w1", "write", {"path": "../escape.txt", "content": "x"}),
                     ("w2", "write", {"path": str(tmp_path / "abs.txt"), "content": "x"}),
                     ("e1", "edit", {"path": "link/out.txt", "old_string": "keep", "new_string": "x"}),
                     ("w3", "write", {"path": "sub/../in.txt", "content": "in"}),
                     ("s1", "shell", {"command": "echo shell-ok"})])
    api.reply("ok")
    p = api.run(work, "-p", "go")
    assert p.returncode == 0, p.stderr
    r = api.results(mock.requests[1])
    assert r["w1"] == (OUTSIDE, True) and r["w2"] == (OUTSIDE, True) and r["e1"] == (OUTSIDE, True)
    assert not (tmp_path / "escape.txt").exists() and not (tmp_path / "abs.txt").exists()
    assert (tmp_path / "out.txt").read_text() == "keep"
    assert r["w3"][1] is True  # sub/ does not exist: inside, but the write itself fails
    assert r["s1"] == ("shell-ok\n[exit 0]", False)  # auto still covers shell


def test_auto_covers_paths_inside_cwd(mock, tmp_path):
    (tmp_path / "sub").mkdir()
    api = Api(mock, "local")
    api.reply(calls=[("w", "write", {"path": "sub/../in.txt", "content": "in"}),
                     ("a", "write", {"path": str(tmp_path / "sub/abs.txt"), "content": "abs"})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    r = api.results(mock.requests[1])
    assert not r["w"][1] and not r["a"][1]
    assert (tmp_path / "in.txt").read_text() == "in" and (tmp_path / "sub/abs.txt").read_text() == "abs"


# ---- edit on a file that is not UTF-8 ----

def test_edit_explains_non_utf8_mismatch(mock, tmp_path):
    (tmp_path / "l1.txt").write_bytes(b"caf\xe9 au lait\n")
    api = Api(mock, "local")
    api.reply(calls=[("e1", "edit", {"path": "l1.txt", "old_string": "caf\ufffd", "new_string": "x"}),
                     ("e2", "edit", {"path": "l1.txt", "old_string": "au lait", "new_string": "noir"})])
    api.reply("ok")
    api.run(tmp_path, "-p", "go")
    r = api.results(mock.requests[1])
    assert r["e1"][1] and "not valid UTF-8" in r["e1"][0] and "shell" in r["e1"][0]
    assert r["e2"] == ("edited l1.txt", False)  # text around the bad byte is still editable
    assert (tmp_path / "l1.txt").read_bytes() == b"caf\xe9 noir\n"  # the raw byte is kept


def test_read_of_an_endless_pipe_says_it_stopped(mock, tmp_path):
    os.mkfifo(tmp_path / "fifo")
    writer = subprocess.Popen(["sh", "-c", "exec yes > fifo"], cwd=tmp_path, stderr=subprocess.DEVNULL)
    api = Api(mock, "local")
    api.reply(calls=[("r", "read", {"path": "fifo"})])
    api.reply("ok")
    try:
        p = api.run(tmp_path, "-p", "go")
    finally:
        writer.kill()
        writer.wait()
    assert p.returncode == 0, p.stderr
    content, err = api.results(mock.requests[1])["r"]
    assert not err and content.startswith("y\ny\n") and " bytes omitted ...]\n" in content
    assert content.endswith("\n[stopped after 64 MB; the rest was not read]")


def test_version_flag(tmp_path):
    p = subprocess.run([str(AGENT), "-V"], capture_output=True, text=True, timeout=10)
    assert p.returncode == 0 and p.stdout == "myra 0.1.2\n"


@pytest.mark.parametrize("flag", ["-h", "--help"])
def test_help_flag(tmp_path, flag):
    p = subprocess.run([str(AGENT), flag], capture_output=True, text=True, timeout=10)
    assert p.returncode == 0 and p.stdout.startswith("usage:") and "--help" in p.stdout
    assert p.stderr == ""


MUTATIONS = [("w", "write", {"path": "../out.txt", "content": "x"}),
             ("e", "edit", {"path": "f", "old_string": "a", "new_string": "b"}),
             ("s", "shell", {"command": "touch made"}),
             ("r", "read", {"path": "f"})]


def run_mutations(mock, tmp_path, *flags):
    work = tmp_path / "work"
    work.mkdir()
    (work / "f").write_text("a")
    api = Api(mock, "local")
    api.reply(calls=MUTATIONS)
    api.reply("ok")
    p = api.run(work, *flags, "-p", "go")
    assert p.returncode == 0, p.stderr
    return api.results(mock.requests[1]), work


@pytest.mark.parametrize("flags", [["--permissions", "all"], ["--permissions=all"]])
def test_permissions_all_allows_everything(mock, tmp_path, flags):
    r, work = run_mutations(mock, tmp_path, *flags)
    assert not any(err for _, err in r.values())
    assert (tmp_path / "out.txt").read_text() == "x" and (work / "made").exists()
    assert (work / "f").read_text() == "b"


def test_permissions_read_only_refuses_changes(mock, tmp_path):
    r, work = run_mutations(mock, tmp_path, "--permissions", "read-only")
    for k in "wes":
        assert r[k] == ("denied: the agent runs with --permissions read-only", True)
    assert r["r"] == ("a", False)  # read still runs
    assert not (tmp_path / "out.txt").exists() and not (work / "made").exists()
    assert (work / "f").read_text() == "a"


@pytest.mark.parametrize("flags", [[], ["--permissions", "auto"]])
def test_default_is_auto(mock, tmp_path, flags):
    r, work = run_mutations(mock, tmp_path, *flags)
    assert r["w"] == (OUTSIDE, True) and not (tmp_path / "out.txt").exists()
    assert not r["e"][1] and not r["s"][1] and (work / "made").exists()


def test_y_is_gone(tmp_path):
    p = subprocess.run([str(AGENT), "-y", "-p", "x"], cwd=tmp_path, capture_output=True,
                       text=True, timeout=10)
    assert p.returncode == 2 and "usage:" in p.stderr


def test_unknown_permissions_mode(tmp_path):
    p = subprocess.run([str(AGENT), "--permissions", "yolo", "-p", "x"], cwd=tmp_path,
                       capture_output=True, text=True, timeout=10)
    assert p.returncode == 2 and "unknown permissions mode yolo" in p.stderr


def test_call_without_id_gets_one_that_matches(mock, tmp_path):
    (tmp_path / "f").write_text("data")
    mock.replies.append((200, {"_plain": True, "choices": [{"index": 0, "finish_reason": "tool_calls",
        "message": {"role": "assistant", "content": None, "tool_calls": [
            {"type": "function", "function": {"name": "read", "arguments": '{"path": "f"}'}}]}}]}))
    Api(mock, "local").reply("done")
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    msgs = mock.requests[1]["body"]["messages"]
    assert msgs[2]["tool_calls"][0]["id"] == msgs[3]["tool_call_id"] == "myra_call_0"


# ---- tool lines, --verbose, cost, color ----

def test_tool_line_is_one_line(mock, tmp_path):
    api = Api(mock, "local")
    long_cmd = "echo " + "x" * 300
    api.reply(calls=[("a", "shell", {"command": long_cmd}),
                     ("b", "shell", {"command": "echo one\necho two"})])
    api.reply("ok")
    p = api.run(tmp_path, "-p", "go")
    lines = p.stderr.splitlines()
    assert len(lines) == 2  # one per call, and nothing else
    assert lines[0].startswith("[tool] shell echo xxx") and lines[0].endswith(" ...")
    assert len(lines[0]) == 100  # stderr is not a terminal: 100 columns, filled
    assert lines[1] == "[tool] shell echo one ..."


def test_verbose_shows_full_calls_and_results(mock, tmp_path):
    api = Api(mock, "local")
    api.reply(calls=[("b", "shell", {"command": "echo one\necho two"})])
    api.reply("ok")
    p = api.run(tmp_path, "--verbose", "-p", "go")
    assert p.stderr == "[tool] shell echo one\necho two\none\ntwo\n[exit 0]\n"


def test_cost_per_turn_and_session(mock, tmp_path):
    api = Api(mock, "openrouter")
    for text, cost in (("a", 0.0123), ("b", 0.0200)):
        api.reply(text)
        mock.replies[-1][1]["usage"] = {"prompt_tokens": 100, "completion_tokens": 10,
                                        "prompt_tokens_details": {"cached_tokens": 50}, "cost": cost}
    p = api.run(tmp_path, stdin="one\ntwo\n")
    assert "[usage] 1 request: 100 in (50 cached), 10 out, $0.0123\n" in p.stderr
    assert "[usage] 1 request: 100 in (50 cached), 10 out, $0.0200\n" in p.stderr
    assert p.stderr.endswith("[session] 2 requests: 200 in (100 cached), 20 out, $0.0323\n")


def test_no_cost_when_the_server_reports_none(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("a")
    mock.replies[-1][1]["usage"] = {"prompt_tokens": 5, "completion_tokens": 1}
    p = api.run(tmp_path, stdin="one\n")
    assert p.stderr.endswith("[usage] 1 request: 5 in (0 cached), 1 out\n"
                             "[session] 1 request: 5 in (0 cached), 1 out\n")


@pytest.mark.filterwarnings("ignore::DeprecationWarning")
@pytest.mark.parametrize("args,env,colored", [([], {}, True), (["--no-color"], {}, False),
                                              ([], {"NO_COLOR": "1"}, False)])
def test_color_on_a_terminal_only(mock, tmp_path, args, env, colored):
    api = Api(mock, "local")
    api.reply(calls=[("s", "shell", {"command": "true"})])
    api.reply("done")
    tty = Tty(api, tmp_path, *args, env=env)
    tty.expect(b"> ")
    tty.type(b"go\r")
    tty.expect(b"done")
    tty.expect(b"> ")
    tty.type(b"\x04")
    assert tty.wait() == 0
    assert (b"\x1b[36m[tool] shell true" in tty.out) == colored
    assert (b"\x1b[1mmyra 0.1.2" in tty.out) == colored


# ---- limits and stream framing ----

def test_tool_calls_per_turn_are_capped(mock, tmp_path):
    api = Api(mock, "local")
    for i in range(100):  # a model that only ever asks for another tool
        api.reply(calls=[(f"c{i}", "shell", {"command": "true"})])
    api.reply("after")
    p = api.run(tmp_path, stdin="loop\nand now?\n")
    assert p.returncode == 0, p.stderr
    assert "stopped: 100 tool calls in one turn" in p.stderr
    assert len(mock.requests) == 101  # 100 calls, then the next prompt
    assert p.stdout == "after\n"  # the REPL carries on
    assert mock.requests[100]["body"]["messages"][-1]["content"] == "and now?"


@pytest.mark.parametrize("batches,skipped", [([101], 1), ([60, 60], 20)], ids=["one", "crossing"])
def test_tool_call_cap_holds_within_a_batch(mock, tmp_path, batches, skipped):
    api = Api(mock, "local")
    n = 0
    for size in batches:
        api.reply(calls=[(f"c{n + i}", "shell", {"command": f"touch f{n + i}"}) for i in range(size)])
        n += size
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0, p.stderr
    assert f"stopped: 100 tool calls in one turn; skipped {skipped}" in p.stderr
    assert len(mock.requests) == len(batches)  # no request after the cap
    assert len(list(tmp_path.glob("f*"))) == 100 and not (tmp_path / "f100").exists()


def test_models_works_after_an_interrupted_turn(mock, tmp_path):
    api = Api(mock, "local")
    api.reply(calls=[("s", "shell", {"command": "echo $$ > pid; sleep 30"})])
    mock.replies.append((200, MODELS))
    p = start(api, tmp_path)
    p.stdin.write("go\n")
    p.stdin.flush()
    wait_for(lambda: (tmp_path / "pid").exists() and (tmp_path / "pid").read_text().strip())
    p.send_signal(signal.SIGINT)
    wait_for(lambda: pid_gone(tmp_path / "pid"))
    out, err = p.communicate("/models gpt\n", timeout=10)
    assert p.returncode == 0, err
    assert out == "  openai/gpt-5.5\n", err  # the interrupt ended with its turn


def test_last_event_without_a_blank_line_is_kept(mock, tmp_path):
    mock.replies.append((200, {"_sse": ['data: {"choices":[{"index":0,"delta":{"content":"hi"}}]}\n\n',
                                        'data: {"choices":[{"index":0,"delta":{},'
                                        '"finish_reason":"stop"}]}']}))  # no trailing newline
    p = Api(mock, "local").run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "hi\n", p.stderr


def test_cut_off_stream_is_retried(mock, tmp_path):
    api = Api(mock, "local")  # events, but no finish_reason and no [DONE]
    mock.replies.append((200, sse({"role": "assistant"}, done=False)))  # nothing printed yet
    api.reply("recovered")
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 0 and p.stdout == "recovered\n"
    assert "stream error, retrying: the stream ended early" in p.stderr
    assert len(mock.requests) == 2


def test_cut_off_stream_after_text_is_not_retried(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((200, {"_sse": ['data: {"choices":[{"index":0,"delta":'
                                        '{"content":"half an ans"}}]}\n\n']}))
    p = api.run(tmp_path, "-p", "go")
    assert p.returncode == 1 and p.stdout == "half an ans\n"
    assert "the stream ended early" in p.stderr and "retrying" not in p.stderr
    assert len(mock.requests) == 1


@pytest.mark.parametrize("stdin", ["/models\n", "hi\n"], ids=["get", "post"])
def test_oversized_response_is_refused(mock, tmp_path, stdin):
    # Plain JSON either way: the POST takes the streaming path with no events, which keeps all.
    mock.replies.append((200, {"_plain": True, "data": [{"id": "x"}], "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "ok"}, "finish_reason": "stop"}],
        "pad": "a" * (64 * 1024 * 1024)}))
    p = Api(mock, "local").run(tmp_path, stdin=stdin)
    assert p.returncode == 0, p.stderr
    assert "error: response exceeds 64 MB" in p.stderr and "retrying" not in p.stderr
    assert p.stdout == "" and len(mock.requests) == 1


@pytest.mark.parametrize("value", ["abc", "0", "-5", ""])
def test_bad_retry_delay_falls_back_to_default(mock, tmp_path, value):
    api = Api(mock, "local")
    mock.replies.append((503, {"error": {}}))
    api.reply("ok")
    t = time.monotonic()
    p = api.run(tmp_path, "-p", "go", env={"MYRA_RETRY_DELAY_MS": value})
    assert p.returncode == 0 and 0.9 < time.monotonic() - t < 3  # the 1 s default
