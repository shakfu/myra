"""End-to-end tests: run ./agent against a scripted mock of /chat/completions."""

import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

AGENT = Path(os.environ.get("AGENT_BIN") or Path(__file__).resolve().parent.parent / "build/agent")

# provider -> (key env, base-url env, base-url suffix)
PROVIDERS = {
    "openrouter": ("OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "/api/v1"),
    "local": ("LOCAL_API_KEY", "LOCAL_BASE_URL", "/v1"),
}


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
    """Runs the agent against the mock as one provider."""

    def __init__(self, mock, provider):
        self.mock, self.provider = mock, provider

    def env(self, cwd, key="test-key"):
        key_env, base_env, suffix = PROVIDERS[self.provider]
        env = {"PATH": "/usr/bin:/bin", base_env: self.mock.url + suffix,
               "XDG_STATE_HOME": str(cwd / ".state")}
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
    p = api.run(tmp_path, "-y", "-p", "go")
    assert p.returncode == 0, p.stderr
    assert p.stdout == "working\ndone\n"
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
    p = api.run(tmp_path, "-y", "-p", "go")
    assert p.returncode == 0, p.stderr
    content, err = api.results(api.mock.requests[1])["e"]
    assert err and msg in content
    assert (tmp_path / "b.txt").read_text() == "x x\n"


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
    p = api.run(tmp_path, "-y", stdin="a\nb\n")
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
    assert (tmp_path / ".state/ant/openrouter.model").read_text() == "openai/gpt-5.5\n"
    api.reply("b")
    assert api.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[1]["body"]["model"] == "openai/gpt-5.5"

    local = Api(mock, "local")  # other provider keeps its own default
    local.reply("c")
    assert local.run(tmp_path, "-p", "x").returncode == 0
    assert mock.requests[2]["body"]["model"] == "local"

    api.reply("d")  # a new -m replaces the remembered one
    assert api.run(tmp_path, "-m", "anthropic/claude-opus-4.8", "-p", "x").returncode == 0
    assert (tmp_path / ".state/ant/openrouter.model").read_text() == "anthropic/claude-opus-4.8\n"


def test_rejected_model_is_not_remembered(mock, tmp_path):
    api = Api(mock, "openrouter")
    mock.replies.append((400, {"error": {"message": "no such model"}}))
    assert api.run(tmp_path, "-m", "typo", "-p", "x").returncode == 1
    assert not (tmp_path / ".state/ant/openrouter.model").exists()
    api.reply("hi")
    api.run(tmp_path, "-p", "x")
    assert mock.requests[1]["body"]["model"] == "anthropic/claude-opus-5"


def test_blank_state_file_uses_default(mock, tmp_path):
    (tmp_path / ".state/ant").mkdir(parents=True)
    (tmp_path / ".state/ant/local.model").write_text(" \n")
    api = Api(mock, "local")
    api.reply("hi")
    api.run(tmp_path, "-p", "x")
    assert mock.requests[0]["body"]["model"] == "local"


def test_state_falls_back_to_home(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    p = api.run(tmp_path, "-m", "qwen", "-p", "x", env={"XDG_STATE_HOME": "", "HOME": str(tmp_path)})
    assert p.returncode == 0, p.stderr
    assert (tmp_path / ".local/state/ant/local.model").read_text() == "qwen\n"


def test_unwritable_state_warns_but_runs(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("hi")
    p = api.run(tmp_path, "-m", "qwen", "-p", "x", env={"XDG_STATE_HOME": "/dev/null/x"})
    assert p.returncode == 0 and p.stdout == "hi\n"
    assert "cannot save /dev/null/x/ant/" in p.stderr


def test_repl_shows_provider_and_model(mock, tmp_path):
    api = Api(mock, "openrouter")
    p = api.run(tmp_path, stdin="")
    assert p.returncode == 0
    assert p.stderr.startswith("openrouter anthropic/claude-opus-5\n")


# ---- provider selection: -P, else first cloud key set, else error ----

def both_keys(tmp_path, mock):
    """Env where either provider could run: OpenRouter key set, both base URLs at the mock."""
    return {**Api(mock, "local").env(tmp_path), **Api(mock, "openrouter").env(tmp_path)}


def run_plain(tmp_path, env, *args):
    return subprocess.run([str(AGENT), *args], cwd=tmp_path, env=env, text=True,
                          capture_output=True, timeout=30, start_new_session=True)


def test_provider_is_not_remembered(mock, tmp_path):
    env = both_keys(tmp_path, mock)
    Api(mock, "local").reply("a")
    assert run_plain(tmp_path, env, "-P", "local", "-p", "x").returncode == 0
    Api(mock, "openrouter").reply("b")  # key is set, so openrouter, not the last -P
    assert run_plain(tmp_path, env, "-p", "x").returncode == 0
    assert [r["path"] for r in mock.requests] == ["/v1/chat/completions",
                                                  "/api/v1/chat/completions"]
    assert not (tmp_path / ".state/ant/provider").exists()


def test_stale_provider_file_is_ignored(mock, tmp_path):
    (tmp_path / ".state/ant").mkdir(parents=True)
    (tmp_path / ".state/ant/provider").write_text("local\n")
    p = Api(mock, "local").run(tmp_path, "-p", "x", provider_flag=False, key=None)
    assert p.returncode == 1 and "set OPENROUTER_API_KEY or pass -P local" in p.stderr
    assert not mock.requests


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
    assert (tmp_path / ".state/ant/local.model").read_text() == "qwen\n"


def test_model_command_not_saved_until_accepted(mock, tmp_path):
    api = Api(mock, "local")
    mock.replies.append((400, {"error": {"message": "no such model"}}))
    p = api.run(tmp_path, stdin="/model typo\nhi\n")
    assert "no such model" in p.stderr
    assert not (tmp_path / ".state/ant/local.model").exists()


def test_model_prefix_is_not_a_command(mock, tmp_path):
    api = Api(mock, "local")
    api.reply("ok")
    api.run(tmp_path, stdin="/modelling question\n")
    assert mock.requests[0]["method"] == "POST"
    assert mock.requests[0]["body"]["messages"][1]["content"] == "/modelling question"


# ---- /models ----

MODELS = {"object": "list", "data": [{"id": "anthropic/claude-opus-5"}, {"id": "openai/gpt-5.5"},
                                     {"id": "anthropic/claude-sonnet-5"}, {"name": "no id"}]}


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
                        "* anthropic/claude-opus-5\n  anthropic/claude-sonnet-5\n")


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
