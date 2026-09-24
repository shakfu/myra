#!/usr/bin/env python3
"""agent.py - myra in Python 3.11+, stdlib only: providers, the four tools, the tool loop and the REPL.

Behaves as ./myra does and shares its saved state; tests/test_agent.py runs against either.
See README.md for the user-facing behaviour, and docs/dev/divergences.md for where the two differ.
"""

from __future__ import annotations

import argparse
import codecs
import copy
import http.client
import json
import os
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from contextlib import contextmanager, suppress
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import Any

try:
    import readline
except ImportError:  # no line editing, as when input is piped
    readline = None

VERSION = "0.2.0"
SHELL_TIMEOUT = 120  # seconds, when the model gives none
SHELL_TIMEOUT_MAX = 600
MAX_RAW = 64 * 1024 * 1024  # bytes read before a tool gives up
MAX_RESPONSE = 64 * 1024 * 1024  # bytes received before a request gives up
MAX_TOOL_CALLS = 100  # per turn, so a looping model stops
RETRIES = 4
HISTORY_MAX = 1000

TOOLS = [
    {"type": "function", "function": {
        "name": "read", "description": "Read a text file and return its contents.",
        "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                       "required": ["path"], "additionalProperties": False}}},
    {"type": "function", "function": {
        "name": "write", "description": "Create or overwrite a file with the given content.",
        "parameters": {"type": "object",
                       "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                       "required": ["path", "content"], "additionalProperties": False}}},
    {"type": "function", "function": {
        "name": "edit",
        "description": "Replace old_string with new_string in a file. "
                       "old_string must occur exactly once.",
        "parameters": {"type": "object",
                       "properties": {"path": {"type": "string"}, "old_string": {"type": "string"},
                                      "new_string": {"type": "string"}},
                       "required": ["path", "old_string", "new_string"],
                       "additionalProperties": False}}},
    {"type": "function", "function": {
        "name": "shell",
        "description": "Run a command with /bin/sh in the working directory. "
                       "Returns combined stdout and stderr and the exit status. stdin is /dev/null. "
                       "The command and its children are killed after timeout seconds "
                       "(default 120, max 600). "
                       "Background jobs must redirect their output, or the call waits for them.",
        "parameters": {"type": "object",
                       "properties": {"command": {"type": "string"}, "timeout": {"type": "integer"}},
                       "required": ["command"], "additionalProperties": False}}},
]

COMMANDS = [
    ("/model", " [id]", "show or switch the model"),
    ("/models", " [filter]", "list the provider's models whose id contains filter"),
    ("/clear", "", "start a new conversation"),
    ("/help", "", "show this list"),
    ("/exit", "", "quit (also Ctrl-D)"),
]

# ---- output ----

color = False
interrupted = False  # set by SIGINT; ask() clears it
raising = False  # whether SIGINT also raises KeyboardInterrupt; see interruptible()


class Style(StrEnum):
    PLAIN = ""
    TOOL = "\033[36m"
    ERROR = "\033[31m"
    WARN = "\033[33m"
    DIM = "\033[2m"
    BOLD = "\033[1m"


def note(msg: str, style: Style = Style.PLAIN) -> None:
    """Write msg to stderr, colored by style when color is on."""
    if color and style:
        msg = f"{style}{msg}\033[0m"
    sys.stderr.write(msg)
    sys.stderr.flush()


def on_sigint(sig: int, frame: object) -> None:
    global interrupted
    interrupted = True
    if raising:
        raise KeyboardInterrupt


@contextmanager
def interruptible():
    """Let SIGINT raise KeyboardInterrupt in the block, for blocking waits it must cut short.
    Elsewhere it only sets interrupted, which the shell loop polls, so cleanup cannot be cut off."""
    global raising
    outer, raising = raising, True  # nested: a completion's request runs inside input()
    try:
        yield
    finally:
        raising = outer


# ---- providers and permissions ----

@dataclass(frozen=True)
class Provider:
    name: str
    key_env: str
    base_env: str
    base: str
    model: str
    key_optional: bool = False
    cache_control: bool = False  # send OpenRouter's top-level cache_control
    max_output: int = 16 * 1024  # cap on one tool result, in bytes; MYRA_MAX_OUTPUT overrides


PROVIDERS = {p.name: p for p in (
    Provider("openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL",
             "https://openrouter.ai/api/v1", "anthropic/claude-opus-5",
             cache_control=True, max_output=100 * 1024),
    # Any OpenAI-compatible server; the default URL is llama-server's.
    # 16 KB is ~4-5K tokens: a quarter of llama-server's usual 16K context.
    Provider("local", "LOCAL_API_KEY", "LOCAL_BASE_URL", "http://localhost:8080/v1", "local",
             key_optional=True),
)}


class Permissions(StrEnum):
    """When write, edit and shell may run without asking on the terminal."""
    AUTO = "auto"  # always, except write/edit outside the working directory
    ASK = "ask"  # never: ask each time
    ALL = "all"  # always
    READ_ONLY = "read-only"  # refuse them; read still runs


def has_key(p: Provider) -> bool:
    return bool(os.environ.get(p.key_env))


def default_provider() -> Provider | None:
    """The remembered provider if it can run, else the first cloud provider with a key set."""
    last = PROVIDERS.get(load_state("provider") or "")
    if last and (last.key_optional or has_key(last)):
        return last
    return next((p for p in PROVIDERS.values() if not p.key_optional and has_key(p)), None)


# ---- remembered state ----

def state_path(name: str, make_dir: bool = False) -> Path | None:
    """$XDG_STATE_HOME/myra/<name>, default ~/.local/state/myra/<name>; None without either."""
    if xdg := os.environ.get("XDG_STATE_HOME"):
        base = Path(xdg)
    elif home := os.environ.get("HOME"):
        base = Path(home) / ".local/state"
    else:
        return None
    path = base / "myra" / name
    if make_dir:
        with suppress(OSError):
            path.parent.mkdir(parents=True, exist_ok=True)
    return path


def load_state(name: str) -> str | None:
    path = state_path(name)
    try:
        value = path.read_text(errors="replace").rstrip() if path else ""
    except OSError:
        return None
    return value or None


def store_state(name: str, value: str) -> None:
    if not (path := state_path(name, make_dir=True)):
        return
    try:
        spit(path, f"{value}\n".encode())
    except OSError:
        note(f"warning: cannot save {path}\n", Style.WARN)


# ---- files ----

def dangling_symlink(path: str | Path) -> bool:
    """A symlink whose target does not exist. Writing would replace the link itself, since
    realpath fails on it and the path then looks like a new file."""
    return os.path.islink(path) and not os.path.exists(path)


def spit(path: str | Path, data: bytes) -> None:
    """Replace path's contents atomically: a failed write leaves the old file intact.

    Writes a temporary file beside the target (symlinks followed, mode kept) and renames it
    over; a hard-linked file thus gets its own copy. A dangling symlink is refused, not
    replaced by a regular file. Where no temporary file can be made, e.g. a read-only
    directory holding a writable file, it writes in place. Raises OSError.
    """
    try:
        target = os.path.realpath(path, strict=True)
    except OSError:
        if dangling_symlink(path):
            raise  # the rename would eat the link
        target = os.fspath(path)  # a new file: as given
    try:
        mode = stat.S_IMODE(os.stat(target).st_mode)
    except OSError:
        mask = os.umask(0)
        os.umask(mask)
        mode = 0o666 & ~mask
    try:
        fd, tmp = tempfile.mkstemp(prefix=".myra-tmp-", dir=os.path.dirname(target) or ".")
    except OSError:  # in place, as a last resort
        with open(target, "wb") as f:
            f.write(data)
        return
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fchmod(f.fileno(), mode)
            os.fsync(f.fileno())
        # The directory is not fsync'ed: the rename survives a crash, not necessarily power loss.
        os.replace(tmp, target)
    except BaseException:
        with suppress(OSError):
            os.unlink(tmp)
        raise


def clean(b: bytes) -> str:
    """b as text: invalid UTF-8 and NUL become U+FFFD, so the JSON request stays valid."""
    return b.decode("utf-8", "replace").replace("\0", "�")


class Sink:
    """Collects output, keeping only what will be shown: the first fifth of cap and the last
    four fifths. Memory therefore stays at cap however much arrives."""

    def __init__(self, cap: int):
        self.head_max = cap // 5
        self.tail_cap = cap - self.head_max
        self.head, self.tail = bytearray(), bytearray()
        self.total = 0

    def add(self, data: bytes) -> None:
        self.total += len(data)
        if (room := self.head_max - len(self.head)) > 0:
            self.head += data[:room]
            data = data[room:]
        self.tail += data
        del self.tail[:-self.tail_cap]

    def take(self) -> str:
        """Head, an omitted-bytes note when anything was dropped, then tail. Both parts
        are cut on character boundaries."""
        head, tail = bytes(self.head), bytes(self.tail)
        dropped = self.total - len(head) - len(tail)
        if not dropped:
            return clean(head + tail)
        dec = codecs.getincrementaldecoder("utf-8")("replace")
        dec.decode(head)  # holds back a trailing partial character
        cut = len(dec.getstate()[0])
        skip = len(tail) - len(tail.lstrip(bytes(range(0x80, 0xC0))))  # continuation bytes
        head, tail = head[:len(head) - cut], tail[skip:]
        dropped += cut + skip
        return f"{clean(head)}\n[... {dropped} bytes omitted ...]\n{clean(tail)}"


# ---- tools: each returns (result, is_error) ----

def tool_read(path: str, cap: int) -> tuple[str, bool]:
    sink = Sink(cap)
    try:
        with open(path, "rb") as f:
            st = os.fstat(f.fileno())
            size, regular = st.st_size, stat.S_ISREG(st.st_mode)
            if regular and size > cap:  # skip the middle
                sink.add(f.read(sink.head_max))
                f.seek(size - sink.tail_cap)
                sink.add(f.read(sink.tail_cap))
                sink.total = size
            else:
                while sink.total < MAX_RAW and (chunk := f.read(65536)):
                    sink.add(chunk)
    except OSError:
        return f"cannot read {path}", True
    if b"\0" in sink.head or b"\0" in sink.tail:
        return f"{path} is binary", True
    out = sink.take()
    if not regular and sink.total >= MAX_RAW:  # a pipe or device outlasted the limit
        nl = "\n" if out and not out.endswith("\n") else ""
        out += f"{nl}[stopped after {MAX_RAW // (1024 * 1024)} MB; the rest was not read]"
    return out, False


def tool_write(path: str, content: str) -> tuple[str, bool]:
    if dangling_symlink(path):
        return f"{path} is a symlink to a missing file", True
    try:
        spit(path, content.encode())
    except OSError:
        return f"cannot write {path}", True
    return f"wrote {path}", False


def tool_edit(path: str, old: str, new: str) -> tuple[str, bool]:
    if not old:
        return "old_string is empty", True
    if dangling_symlink(path):
        return f"{path} is a symlink to a missing file", True
    with suppress(OSError):
        if os.stat(path).st_size > MAX_RAW:  # edit holds the whole file
            return f"{path} is too large to edit; use shell (sed, awk)", True
    try:
        data = Path(path).read_bytes()
    except OSError:
        return f"cannot read {path}", True
    if b"\0" in data:  # read refuses these; myra cannot search them safely (strstr)
        return f"{path} is binary", True
    old_b = old.encode()
    hit = data.find(old_b)
    if hit < 0 and "�" in old:  # U+FFFD from read
        try:
            data.decode()
        except UnicodeDecodeError:
            return (f"old_string not found in {path}: the file is not valid UTF-8, and read "
                    "showed its invalid bytes as U+FFFD, which edit cannot match. "
                    "Use shell (sed, iconv)."), True
    if hit < 0:
        return f"old_string not found in {path}", True
    if data.find(old_b, hit + 1) >= 0:
        return f"old_string occurs more than once in {path}", True
    try:
        spit(path, data[:hit] + new.encode() + data[hit + len(old_b):])
    except OSError:
        return f"cannot write {path}", True
    return f"edited {path}", False


def kill_group(proc: subprocess.Popen) -> None:
    """SIGTERM the process group, SIGKILL whatever is left after 2 s."""
    with suppress(OSError):
        os.killpg(proc.pid, signal.SIGTERM)
    with suppress(subprocess.TimeoutExpired):
        proc.wait(timeout=2)
    with suppress(OSError):
        os.killpg(proc.pid, signal.SIGKILL)  # also children that outlived sh
    proc.wait()


def tool_shell(cmd: str, timeout: int, cap: int) -> tuple[str, bool]:
    # sh runs in its own process group, so the terminal's Ctrl-C reaches only the agent,
    # which then decides to kill the group.
    try:
        proc = subprocess.Popen(["/bin/sh", "-c", cmd], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, process_group=0)
    except OSError as e:
        return f"cannot run /bin/sh: {e.strerror}", True
    sink, stopped = Sink(cap), ""
    deadline = time.monotonic() + timeout
    fd = proc.stdout.fileno()
    try:
        while True:
            if interrupted:
                stopped = "interrupted by user"
                break
            if sink.total > MAX_RAW:
                stopped = f"stopped after {MAX_RAW // (1024 * 1024)} MB of output"
                break
            if (left := deadline - time.monotonic()) <= 0:
                stopped = f"timed out after {timeout}s"
                break
            if select.select([fd], [], [], min(left, 0.2))[0]:  # the tick rechecks interrupted
                if not (chunk := os.read(fd, 65536)):
                    break  # EOF: every writer has exited
                sink.add(chunk)
        # EOF: sh may have closed its output and kept running.
        while not stopped and proc.poll() is None:
            if interrupted:
                stopped = "interrupted by user"
            elif time.monotonic() >= deadline:
                stopped = f"timed out after {timeout}s"
            else:
                time.sleep(0.01)
    finally:
        proc.stdout.close()
        if stopped:
            kill_group(proc)
    out = sink.take()
    nl = "\n" if out and not out.endswith("\n") else ""
    code = proc.returncode if proc.returncode >= 0 else 128 - proc.returncode
    if stopped:
        return f"{out}{nl}[{stopped}; killed]", True
    return f"{out}{nl}[exit {code}]", code != 0


def outside_cwd(path: str) -> bool:
    """Whether path, with symlinks resolved, lies outside the working directory. A path
    whose directory does not exist counts as inside: writing there fails anyway."""
    cwd = os.path.realpath(".")
    try:
        real = os.path.realpath(path, strict=True)
    except OSError:  # a new file: resolve its directory
        try:
            real = os.path.realpath(os.path.dirname(path) or ".", strict=True)
        except OSError:
            return False
    return cwd != "/" and os.path.commonpath([cwd, real]) != cwd


def denied(mode: Permissions, outside: bool, name: str, detail: str) -> str | None:
    """None if allowed, else why not, for the model to relay. Asks on the controlling
    terminal so it works in headless mode and the REPL alike."""
    if mode is Permissions.READ_ONLY:
        return "denied: the agent runs with --permissions read-only"
    if mode is Permissions.ALL or (mode is Permissions.AUTO and not outside):
        return None
    try:
        tty = open("/dev/tty", "r+")  # noqa: SIM115 - closed by `with tty`; the try is for open only
    except OSError:
        if mode is Permissions.AUTO:
            return "denied: outside the working directory; --permissions all allows it"
        return "denied: no terminal to confirm; --permissions auto skips asking"
    with tty:
        where = " outside the working directory" if outside else ""
        tty.write(f"allow {name}{where}: {detail} ? [y/N] ")
        tty.flush()
        try:
            with interruptible():
                ok = tty.readline(16)[:1] in ("y", "Y")
        except KeyboardInterrupt:  # interrupted is set: the turn's remaining calls are skipped
            ok = False
    return None if ok else "denied by user"


# ---- JSON from the server: a trust boundary, so every field is type-checked ----

def num(obj: Any, key: str) -> float:
    """obj[key] if it is a number, else 0."""
    v = obj.get(key) if isinstance(obj, dict) else None
    return v if isinstance(v, int | float) and not isinstance(v, bool) else 0


def first_choice(resp: Any) -> dict:
    choices = resp.get("choices") if isinstance(resp, dict) else None
    ok = isinstance(choices, list) and choices and isinstance(choices[0], dict)
    return choices[0] if ok else {}


def compact(obj: Any) -> str:
    return json.dumps(obj, separators=(",", ":"))


def is_context_error(body: str) -> bool:
    """OpenAI's code and llama-server's type first; wording, which varies by server, after."""
    try:
        j = json.loads(body)
    except ValueError:
        j = None
    e = j.get("error") if isinstance(j, dict) else None
    e = e if isinstance(e, dict) else j if isinstance(j, dict) else {}  # mid-stream: the error itself
    if e.get("code") == "context_length_exceeded" or e.get("type") == "exceed_context_size_error":
        return True
    body = body.lower()
    return any(h in body for h in ("context size", "context length", "context window",
                                   "maximum context", "prompt is too long", "too many tokens"))


@dataclass
class Usage:
    """Token counts and cost, as the server reports them."""
    requests: int = 0
    tokens_in: float = 0
    cached: float = 0
    out: float = 0
    cost: float = 0
    costed: bool = False  # the server reported a cost: OpenRouter does, llama-server does not

    def add(self, usage: Any) -> None:
        if not isinstance(usage, dict):
            return
        self.requests += 1
        self.tokens_in += num(usage, "prompt_tokens")
        self.out += num(usage, "completion_tokens")
        self.cached += num(usage.get("prompt_tokens_details"), "cached_tokens")
        if "cost" in usage and isinstance(usage["cost"], int | float):
            self.cost += num(usage, "cost")  # OpenRouter credits, which are dollars
            self.costed = not isinstance(usage["cost"], bool)

    def report(self, label: str) -> None:
        if not self.requests:
            return
        s = "" if self.requests == 1 else "s"
        cost = f", ${self.cost:.4f}" if self.costed else ""
        note(f"{label} {self.requests} request{s}: {self.tokens_in:.0f} in "
             f"({self.cached:.0f} cached), {self.out:.0f} out{cost}\n", Style.DIM)


# ---- streaming (server-sent events) ----

class Stream:
    """One streamed reply, rebuilt into the shape of a non-streamed one."""

    def __init__(self):
        self.raw = bytearray()  # the body before any event: servers that ignore "stream"
        self.content: list[str] = []
        self.extra: dict[str, list[str]] = {}  # other text deltas: reasoning, refusal
        self.calls: list[dict] = []  # tool calls; arguments are kept in args
        self.args: list[list[str]] = []
        self.details: list[dict] = []  # reasoning_details, merged by index
        self.usage: dict | None = None
        self.error: str | None = None  # a mid-stream failure, as compact JSON
        self.finish = ""
        self.events = 0
        self.received = 0
        self.printed = self.done = False  # done: the server sent [DONE]

    def feed(self, line: bytes) -> None:
        if not self.events:
            self.raw += line
        line = line.rstrip(b"\r\n")
        if not line.startswith(b"data:"):  # comments (":") and other fields are ignored
            return
        data = line[5:].removeprefix(b" ")
        if data == b"[DONE]":
            self.done = True
        else:
            self.event(data)

    def event(self, data: bytes) -> None:
        try:
            chunk = json.loads(data)
        except ValueError:
            return
        if not isinstance(chunk, dict):
            return
        if "error" in chunk:
            self.error = compact(chunk.pop("error"))
        self.events += 1
        if isinstance(usage := chunk.get("usage"), dict):
            self.usage = usage
        choice = first_choice(chunk)
        if isinstance(finish := choice.get("finish_reason"), str):
            self.finish = finish
        delta = choice.get("delta")
        for k, v in (delta.items() if isinstance(delta, dict) else ()):
            match k, v:
                case "content", str() if v:
                    sys.stdout.write(v)
                    sys.stdout.flush()
                    self.printed = True
                    self.content.append(v)
                case "tool_calls", list():
                    self.merge_calls(v)
                case "reasoning_details", list():
                    self.merge_details(v)
                case _, str() if v and k != "role":
                    self.extra.setdefault(k, []).append(v)

    def merge_calls(self, deltas: list) -> None:
        """Tool calls arrive in pieces keyed by index: arguments are appended, the rest is set."""
        for d in deltas:
            if not isinstance(d, dict) or not 0 <= (idx := num(d, "index")) <= 255:
                continue
            idx = int(idx)
            while len(self.calls) <= idx:
                self.calls.append({"id": "", "type": "function", "function": {"name": ""}})
                self.args.append([])
            fn = d.get("function") if isinstance(d.get("function"), dict) else {}
            if isinstance(id_ := d.get("id"), str) and id_:
                self.calls[idx]["id"] = id_
            if isinstance(name := fn.get("name"), str) and name:
                self.calls[idx]["function"]["name"] = name
            if isinstance(args := fn.get("arguments"), str):
                self.args[idx].append(args)

    def merge_details(self, deltas: list) -> None:
        """reasoning_details pieces: text fields are appended, others set, merged by index."""
        for d in deltas:
            if not isinstance(d, dict):
                continue
            idx = d.get("index")
            into = None
            if isinstance(idx, int | float):
                into = next((e for e in reversed(self.details) if e.get("index") == idx), None)
            if into is None:
                self.details.append(d)
                continue
            for k, v in d.items():
                old = into.get(k)
                into[k] = old + v if isinstance(v, str) and isinstance(old, str) and k != "type" else v

    def result(self) -> dict:
        """The streamed reply as a non-streamed response object."""
        content = "".join(self.content)
        msg: dict[str, Any] = {"role": "assistant",
                               "content": content or (None if self.calls else "")}
        msg |= {k: "".join(v) for k, v in self.extra.items()}
        if self.details:
            msg["reasoning_details"] = self.details
        for call, args in zip(self.calls, self.args):
            call["function"]["arguments"] = "".join(args)
        if self.calls:
            msg["tool_calls"] = self.calls
        choice: dict[str, Any] = {"message": msg}
        if self.finish:
            choice["finish_reason"] = self.finish
        resp: dict[str, Any] = {"choices": [choice]}
        if self.usage is not None:
            resp["usage"] = self.usage
        return resp


def retry_delay() -> float:
    """First back-off in seconds; it doubles each retry."""
    try:
        ms = int(os.environ.get("MYRA_RETRY_DELAY_MS", ""))
    except ValueError:
        ms = 0
    return (1000 if ms < 1 else min(ms, 60000)) / 1000  # a minute of back-off is plenty


# ---- the agent ----

@dataclass
class Agent:
    prov: Provider
    model: str
    api_key: str
    permissions: Permissions
    max_output: int  # cap on one tool result, in bytes
    save_model: bool = False  # remember model after the next successful response
    save_provider: bool = False  # remember provider likewise; set for an explicit -P
    verbose: bool = False  # full tool arguments and results on stderr
    context_full: bool = False  # the last request failed as too long for the model's context
    streamed: bool = False  # the last reply's text was printed while it streamed
    turn_calls: int = 0
    session: Usage = field(default_factory=Usage)
    messages: list[dict] = field(default_factory=list)

    @classmethod
    def create(cls, p: Provider, model: str | None, perms: Permissions) -> Agent | None:
        """model None means the remembered one, else the provider default. None if the key is unset."""
        key = os.environ.get(p.key_env, "")
        if not key and not p.key_optional:
            note(f"error: {p.key_env} is not set\n", Style.ERROR)
            return None
        try:
            cap = int(os.environ.get("MYRA_MAX_OUTPUT", ""))
        except ValueError:
            cap = 0
        remembered = load_state(f"{p.name}.model")
        a = cls(p, model or remembered or p.model, key, perms,
                # read takes the whole-file path for a file up to the cap, which stops at MAX_RAW
                min(cap if cap >= 1024 else p.max_output, MAX_RAW),
                save_model=model is not None and model != remembered)
        a.messages.append({"role": "system", "content": (
            f"You are a coding agent. The working directory is {os.getcwd()}. Use the read, "
            "write, edit and shell tools to inspect and change code. Verify changes where you "
            "can. Be concise.")})
        return a

    def clear(self) -> None:
        """Drop the conversation, keeping the system prompt."""
        del self.messages[1:]

    def set_model(self, model: str) -> None:
        """Switch model; it is remembered after the next successful response."""
        self.model = model
        self.save_model = True

    # ---- API ----

    def request(self, path: str, body: dict | None = None) -> Any:
        """POST body to path, streamed, or GET path when body is None. Retries 429, 5xx and
        network errors, unless streamed text was already printed. None on failure."""
        url = (os.environ.get(self.prov.base_env) or self.prov.base) + path
        headers = {"content-type": "application/json"}
        if self.api_key:
            headers["authorization"] = f"Bearer {self.api_key}"
        data = json.dumps(body).encode() if body is not None else None
        delay = retry_delay()
        self.streamed = self.context_full = False
        for attempt in range(RETRIES + 1):
            st, status, neterr = Stream(), 0, None
            try:
                with interruptible():
                    if attempt:
                        time.sleep(delay * 2 ** (attempt - 1))
                    if interrupted:  # before the block, or during the sleep's last instant
                        raise KeyboardInterrupt
                    try:
                        req = urllib.request.Request(url, data, headers)
                        with urllib.request.urlopen(req, timeout=600) as r:
                            status = r.status
                            if data is None or status != 200:
                                st.raw += r.read(MAX_RESPONSE + 1)
                                st.received = len(st.raw)
                            else:
                                while line := r.readline(MAX_RESPONSE + 1 - st.received):
                                    st.received += len(line)
                                    if st.received > MAX_RESPONSE:
                                        break
                                    st.feed(line)
                    except urllib.error.HTTPError as e:
                        status = e.code
                        with e, suppress(OSError, http.client.HTTPException):
                            st.raw += e.read(MAX_RESPONSE + 1)
                            st.received = len(st.raw)
                    except urllib.error.URLError as e:
                        if isinstance(e.reason, ConnectionRefusedError):  # retrying cannot help
                            note(f"error: cannot connect to {url}; is the server running?\n",
                                 Style.ERROR)
                            return None
                        neterr = str(e.reason)
                    except (OSError, http.client.HTTPException) as e:
                        neterr = str(e) or type(e).__name__
            except KeyboardInterrupt:
                note("interrupted\n", Style.WARN)
                return None
            finally:
                if st.printed:  # end the streamed line
                    sys.stdout.write("\n")
                    sys.stdout.flush()
                self.streamed = st.printed

            if st.received > MAX_RESPONSE:  # a retry would receive the same
                note(f"error: response exceeds {MAX_RESPONSE // (1024 * 1024)} MB\n", Style.ERROR)
                return None
            # No finish_reason and no [DONE]: the stream was cut off, so the reply is partial.
            truncated = bool(st.events) and not st.finish and not st.done
            if neterr is None and status == 200 and st.error is None and not truncated:
                if st.events:
                    return st.result()
                try:
                    return json.loads(st.raw)  # "stream" ignored
                except ValueError:
                    note("error: response is not JSON\n", Style.ERROR)
                    return None
            out = st.error or ("the stream ended early" if truncated else clean(st.raw))
            self.context_full = (status == 400 or st.error is not None) and is_context_error(out)
            retryable = ((neterr is not None or status == 429 or status >= 500
                          or st.error is not None or truncated)
                         and not st.printed and not self.context_full)
            again = ", retrying" if retryable and attempt < RETRIES else ""
            if st.error is not None or truncated:
                note(f"stream error{again}: {out}\n", Style.ERROR)
            elif neterr is not None:
                note(f"api error ({neterr}){again}\n", Style.ERROR)
            else:
                note(f"api error (http {status}){again}: {out}\n", Style.ERROR)
            if not retryable:
                return None
        return None

    def call_api(self) -> Any:
        body = {"model": self.model, "tools": TOOLS, "messages": self.messages, "stream": True,
                "stream_options": {"include_usage": True}}
        # Turns on prompt caching for Anthropic models, where it is opt-in. Providers
        # that cache automatically or lack support ignore unknown fields.
        if self.prov.cache_control:
            body["cache_control"] = {"type": "ephemeral"}
        resp = self.request("/chat/completions", body)
        # Saved only once the server accepts them, so a mistyped choice is not kept.
        if resp is not None and self.save_provider:
            store_state("provider", self.prov.name)
            self.save_provider = False
        if resp is not None and self.save_model:
            store_state(f"{self.prov.name}.model", self.model)
            self.save_model = False
        return resp

    def model_ids(self) -> list[str] | None:
        """The provider's model ids; None if the request failed."""
        if (resp := self.request("/models")) is None:
            return None
        data = resp.get("data") if isinstance(resp, dict) else None
        if not isinstance(data, list):
            note("error: no model list in response\n", Style.ERROR)
            return []
        return [m["id"] for m in data if isinstance(m, dict) and isinstance(m.get("id"), str)]

    def list_models(self, filter_: str) -> None:
        """Print the model ids that contain filter, ignoring case; the current one is starred."""
        for id_ in self.model_ids() or []:
            if filter_.lower() in id_.lower():
                print(f"{'*' if id_ == self.model else ' '} {id_}", flush=True)

    # ---- the loop ----

    def tool_log(self, name: str, detail: str) -> None:
        """One line per call, cut to the terminal width, unless verbose."""
        if self.verbose:
            note(f"[tool] {name} {detail}\n", Style.TOOL)
            return
        width = 100
        if sys.stderr.isatty():
            with suppress(OSError):
                width = os.get_terminal_size(sys.stderr.fileno()).columns or 100
        used = len(f"[tool] {name}  ...")
        room = width - used if width > used + 10 else 10
        line, nl, _ = detail.partition("\n")
        cut = bool(nl) or len(line) > room
        note(f"[tool] {name} {line[:room]}{' ...' if cut else ''}\n", Style.TOOL)

    def run_tool(self, name: str, args: dict) -> tuple[str, bool]:
        match name, args:
            case "read", {"path": str(path)}:
                self.tool_log(name, path)
                return tool_read(path, self.max_output)
            case "write", {"path": str(path), "content": str(content)}:
                self.tool_log(name, path)
                if why := denied(self.permissions, outside_cwd(path), name, path):
                    return why, True
                return tool_write(path, content)
            case "edit", {"path": str(path), "old_string": str(old), "new_string": str(new)}:
                self.tool_log(name, path)
                if why := denied(self.permissions, outside_cwd(path), name, path):
                    return why, True
                return tool_edit(path, old, new)
            case "shell", {"command": str(cmd)}:
                self.tool_log(name, cmd)
                if why := denied(self.permissions, False, name, cmd):  # unconfinable
                    return why, True
                want = args.get("timeout", SHELL_TIMEOUT)
                if not isinstance(want, int | float) or isinstance(want, bool):
                    want = SHELL_TIMEOUT
                secs = 1 if not want >= 1 else min(want, SHELL_TIMEOUT_MAX)  # NaN: 1
                return tool_shell(cmd, int(secs), self.max_output)
        return f"unknown tool or missing arguments: {name}", True

    def step(self, resp: Any) -> int:
        """Consume one chat-completions response: -1 failed, 0 done, 1 tools ran."""
        choice = first_choice(resp)
        msg = choice.pop("message", None)
        if not isinstance(msg, dict):
            note(f"error: no message in response: {compact(resp)}\n", Style.ERROR)
            return -1
        finish = choice.get("finish_reason")
        if finish == "content_filter":
            note("refused: content_filter\n", Style.ERROR)
            return -1
        # Calls cut off by length may be truncated JSON, and dropping them keeps the
        # history valid: every tool call must be followed by its result.
        if finish == "length":
            note("warning: reply hit the length limit\n", Style.WARN)
            msg.pop("tool_calls", None)
        content = msg.get("content")
        if not self.streamed and isinstance(content, str) and content:  # else already shown
            print(content, flush=True)
        calls = msg.get("tool_calls")
        calls = [c for c in calls if isinstance(c, dict)] if isinstance(calls, list) else []
        if calls:
            msg["tool_calls"] = calls
        elif not isinstance(content, str):  # null content is only valid with tool_calls
            msg.pop("tool_calls", None)
            msg["content"] = ""
        self.messages.append(msg)
        budget, skipped = MAX_TOOL_CALLS - self.turn_calls, 0  # the cap holds within a batch
        self.turn_calls += len(calls)
        for i, call in enumerate(calls):
            if not (isinstance(call.get("id"), str) and call["id"]):
                call["id"] = f"myra_call_{i}"  # some servers omit it; the result must name its call
            fn = call.get("function") if isinstance(call.get("function"), dict) else {}
            name = fn.get("name") if isinstance(fn.get("name"), str) else None
            raw = fn.get("arguments") if isinstance(fn.get("arguments"), str) else ""
            try:
                args = json.loads(raw)
            except ValueError:
                args = None
            # Every call still gets a result, so the history stays valid.
            if interrupted:
                out, err = "skipped: interrupted by user", True
            elif i >= budget:
                out, err = "skipped: the turn reached its tool call limit", True
                skipped += 1
            elif not isinstance(args, dict):
                out, err = f"invalid JSON arguments for {name or '?'}", True
            else:
                try:
                    out, err = self.run_tool(name or "", args)
                except (UnicodeError, ValueError) as e:  # e.g. a lone surrogate or NUL in a path
                    out, err = f"bad arguments for {name}: {e}", True
            # Chat completions has no is_error field, so mark failures in the text.
            result = f"error: {out}" if err else out
            if self.verbose:
                note(f"{result}\n", Style.DIM)
            self.messages.append({"role": "tool", "tool_call_id": call["id"], "content": result})
        if interrupted:
            note("interrupted\n", Style.WARN)
            return 0  # end the turn without asking the model again
        if skipped:
            note(f"stopped: {MAX_TOOL_CALLS} tool calls in one turn; skipped {skipped}\n", Style.WARN)
            return 0
        return 1 if calls else 0

    def drop_old_tool_output(self) -> int:
        """Blank every tool result except the trailing batch, which the next reply needs.
        Returns how many were blanked."""
        dropped = "[output dropped to fit the context]"
        last = len(self.messages)
        while last > 0 and self.messages[last - 1].get("role") == "tool":
            last -= 1
        count = 0
        for m in self.messages[:last]:
            if m.get("role") == "tool" and isinstance(m.get("content"), str) and m["content"] != dropped:
                m["content"] = dropped
                count += 1
        return count

    def ask(self, text: str) -> int:
        """Run one user turn. -1 on failure, with history rolled back to before the turn."""
        global interrupted
        interrupted = False
        self.turn_calls = 0
        before = len(self.messages)
        self.messages.append({"role": "user", "content": text})
        turn = Usage()
        undropped = None  # history before the first drop, restored if the turn fails
        while True:  # call the model and run tools until it stops asking for them
            resp = self.call_api()
            usage = resp.get("usage") if isinstance(resp, dict) else None
            turn.add(usage)
            self.session.add(usage)
            if resp is None and self.context_full:
                if undropped is None:
                    undropped = copy.deepcopy(self.messages)
                if n := self.drop_old_tool_output():
                    note(f"context full: dropped {n} old tool output{'' if n == 1 else 's'}, "
                         "retrying\n", Style.WARN)
                    continue
                note("hint: the conversation no longer fits the model's context; "
                     "/clear starts a new one\n", Style.WARN)
            if resp is None:
                rc = -1
                break
            if (rc := self.step(resp)) != 1:
                break
            if self.turn_calls >= MAX_TOOL_CALLS:  # a model looping on tools
                note(f"stopped: {self.turn_calls} tool calls in one turn\n", Style.WARN)
                rc = 0
                break
        turn.report("[usage]")
        if rc < 0 and undropped is not None:  # a misread error must not blank the conversation for good
            self.messages = undropped
        if rc < 0:
            del self.messages[before:]
        return rc


# ---- the REPL ----

def command(line: str, name: str) -> str | None:
    """If line is "name" or "name args", return args with leading spaces skipped; else None."""
    if line == name:
        return ""
    if line.startswith(f"{name} "):
        return line[len(name):].lstrip(" ")
    return None


def repl_help(f) -> None:
    f.write("REPL commands:\n")
    for name, args, help_ in COMMANDS:
        f.write(f"  {name + args:<18} {help_}\n")


def complete_path(text: str) -> list[str]:
    dir_, slash, base = text.rpartition("/")
    dir_ += slash  # the directory, keeping its slash
    try:
        names = os.listdir(dir_ or ".")
    except OSError:
        return []
    out = []
    for name in sorted(names):
        if (name.startswith(".") and not base.startswith(".")) or not name.startswith(base):
            continue  # hidden ones only when asked for
        path = dir_ + name
        out.append(path + "/" if os.path.isdir(path) else path)
    return out


class Completer:
    """readline completion: a command at the start of the line, a model id after /model,
    a path anywhere else."""

    def __init__(self, agent: Agent):
        self.agent = agent
        self.models: list[str] | None = None  # the provider's ids: one fetch, on first use
        self.matches: list[str] = []

    def __call__(self, text: str, state: int) -> str | None:
        if state == 0:
            self.matches = self.candidates(readline.get_line_buffer()[:readline.get_endidx()], text)
        return self.matches[state] if state < len(self.matches) else None

    def candidates(self, line: str, text: str) -> list[str]:
        if line.startswith("/") and not any(c in line for c in " \t"):
            names = [name for name, _, _ in COMMANDS]
        elif command(line, "/model") is not None:
            if self.models is None:  # costs a request, so only once and only if asked
                self.models = self.agent.model_ids() or []
            names = self.models
        else:
            return complete_path(text)
        return [n for n in names if n.startswith(text)]


def setup_readline(agent: Agent, hist: Path | None) -> None:
    readline.set_completer(Completer(agent))
    readline.set_completer_delims(" \t")
    if "libedit" in (readline.__doc__ or ""):
        readline.parse_and_bind("bind ^I rl_complete")
    else:
        readline.parse_and_bind("tab: complete")
    if hist:
        with suppress(OSError):
            for line in hist.read_text(errors="replace").splitlines()[-HISTORY_MAX:]:
                readline.add_history(line)


def save_history(hist: Path) -> None:
    """Plain lines, as linenoise writes them, so ./myra can read the same file."""
    n = readline.get_current_history_length()
    lines = [readline.get_history_item(i) for i in range(max(1, n - HISTORY_MAX + 1), n + 1)]
    try:
        fd = os.open(hist, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(fd, 0o600)  # prompts may hold secrets
        with open(fd, "w", errors="replace") as f:
            f.writelines(f"{line}\n" for line in lines if line is not None)
    except OSError:
        note(f"warning: cannot save {hist}\n", Style.WARN)


def read_line(editing: bool, tty: bool) -> str | None:
    """The next input line, trailing whitespace trimmed; None at EOF. Ctrl-C raises."""
    with interruptible():
        if editing:
            try:
                line = input("> ")
            except EOFError:
                return None
        else:
            if tty:
                note("> ")
            if not (line := sys.stdin.readline()):
                return None
    return line.rstrip(" \t\r\n")


def repl(agent: Agent) -> None:
    note(f"myra {VERSION}\n", Style.BOLD)
    note(f"{agent.prov.name} {agent.model}\n", Style.DIM)
    tty = sys.stdin.isatty()  # no prompt or editing when input is piped
    # input() edits only when stdout is a terminal too; it prints the prompt there.
    editing = tty and sys.stdout.isatty() and readline is not None
    hist = state_path("history", make_dir=True) if editing else None
    if editing:
        setup_readline(agent, hist)
    global interrupted
    try:
        while True:
            # An interrupted turn leaves the flag set; cleared here, before completion can fetch.
            interrupted = False
            try:
                line = read_line(editing, tty)
            except KeyboardInterrupt:  # Ctrl-C at the prompt: drop the line
                note("\n")
                continue
            if line is None:  # EOF
                if tty:
                    note("\n")  # leave the prompt's line
                break
            match line:
                case "":
                    continue
                case "/exit":
                    break
                case "/help":
                    repl_help(sys.stderr)
                case "/clear":
                    agent.clear()
                    note("history cleared\n", Style.DIM)
                case _ if (arg := command(line, "/models")) is not None:
                    agent.list_models(arg)
                case _ if (arg := command(line, "/model")) is not None:
                    if arg:
                        agent.set_model(arg)  # history is kept
                    note(f"{agent.prov.name} {agent.model}\n", Style.DIM)
                case _:
                    agent.ask(line)
    finally:
        if hist:
            save_history(hist)


# ---- the CLI ----

def provider_arg(name: str) -> Provider:
    if p := PROVIDERS.get(name):
        return p
    raise argparse.ArgumentTypeError(f"unknown provider {name}")


def permissions_arg(mode: str) -> Permissions:
    try:
        return Permissions(mode)
    except ValueError:
        raise argparse.ArgumentTypeError(f"unknown permissions mode {mode}") from None


def parser() -> argparse.ArgumentParser:
    cmds = "\n".join(f"  {name + args:<18} {help_}" for name, args, help_ in COMMANDS)
    ap = argparse.ArgumentParser(
        description="A minimal code agent with four tools: read, write, edit and shell.",
        epilog=f"REPL commands:\n{cmds}", formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("-P", dest="provider", type=provider_arg, metavar="provider",
                    help="openrouter or local; remembered. Default: the remembered one if usable,\n"
                         "else the first cloud provider whose key is set")
    ap.add_argument("-m", dest="model", metavar="model", help="model id; remembered per provider")
    ap.add_argument("-p", dest="prompt", metavar="prompt",
                    help="run one prompt headless and exit (default: REPL)")
    ap.add_argument("-V", action="version", version=f"myra {VERSION}", help="print the version")
    ap.add_argument("--verbose", action="store_true",
                    help="show each tool call's full arguments and result, not one line")
    ap.add_argument("--no-color", action="store_true",
                    help="plain stderr; also when NO_COLOR is set or stderr is not a terminal")
    ap.add_argument("--permissions", type=permissions_arg, default=Permissions.AUTO,
                    metavar="mode",
                    help="when write, edit and shell run without asking:\n"
                         "  auto       always, except write/edit outside the working directory (default)\n"
                         "  ask        never: ask on the terminal each time\n"
                         "  all        always\n"
                         "  read-only  refuse them; read still runs")
    return ap


def main(argv: list[str] | None = None) -> int:
    global color
    args = parser().parse_args(argv)
    color = not args.no_color and not os.environ.get("NO_COLOR") and sys.stderr.isatty()
    for f in (sys.stdin, sys.stdout):  # invalid bytes in and lone surrogates out: U+FFFD
        f.reconfigure(errors="replace")
    if args.prompt is not None and args.prompt in PROVIDERS:  # -p and -P differ only by case
        note(f"error: -p takes a prompt; did you mean -P {args.prompt}?\n", Style.ERROR)
        return 2

    if not (p := args.provider or default_provider()):
        keys = "".join(f" {q.key_env}" for q in PROVIDERS.values() if not q.key_optional)
        note(f"error: no provider: set{keys} or pass -P local\n", Style.ERROR)
        return 1
    if not (agent := Agent.create(p, args.model, args.permissions)):
        return 1
    agent.verbose = args.verbose
    agent.save_provider = args.provider is not None
    signal.signal(signal.SIGINT, on_sigint)

    if args.prompt is None:
        repl(agent)
        agent.session.report("[session]")
        return 0
    rc = 1 if agent.ask(args.prompt) < 0 else 0
    return 130 if interrupted else rc  # 128 + SIGINT, as a shell would report


if __name__ == "__main__":
    sys.exit(main())
