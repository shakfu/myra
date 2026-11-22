#!/usr/bin/env python3
"""Live test of -r: a real model delegates to two children, waits for both, and adds up.

    scripts/test_recurse.py
    MODEL=openai/gpt-6-luna scripts/test_recurse.py
    BIN=build/agent-py BUDGET=200000 scripts/test_recurse.py --budget-cost 0.05
    scripts/test_recurse.py -P local
    SCENARIO=async scripts/test_recurse.py

The defaults are -P openrouter and MODEL, default anthropic/claude-haiku-5.5: cheap, and a
strict test of whether the family block in the system prompt is followed. Naming -P or -m
in the arguments replaces them. Other arguments go to the root as given; children inherit its
provider and model. Keys come from the environment, as for myra.

Spends tokens: the family is capped at BUDGET tokens (default 300000).

SCENARIO picks the task:
- fork (default): delegate two line counts, wait, add up.
- async: the same, but the root counts a third file itself while the children run. It
  checks that the root worked before its first wait and never polled. Whether a
  finished-child note fires depends on timing: only a tool batch that ends after a child
  finished, before the root waits for it, gets one. So the note count is printed, not
  checked; tests/test_agent.py covers notes deterministically.

The checks cover what the mock tests cannot: whether a model reads the family block and uses
it. They read the root's --json result, its saved session, and the family's files, which
MYRA_KEEP_FAMILY keeps. The run directory is kept and printed.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.abspath(os.environ.get("BIN", os.path.join(ROOT, "build", "myra")))
BUDGET = int(os.environ.get("BUDGET", "300000"))
TIMEOUT = int(os.environ.get("TIMEOUT", "600"))
MODEL = os.environ.get("MODEL", "anthropic/claude-haiku-5.5")

SCENARIO = os.environ.get("SCENARIO", "fork")
if SCENARIO == "fork":
    LINES = {"a.txt": 17, "b.txt": 25}
    PROMPT = (
        "Use child agents for this. Spawn one child to count the lines in a.txt and another to "
        "count the lines in b.txt; do not count them yourself. Collect both answers, then reply "
        "with only their sum, as a number."
    )
elif SCENARIO == "async":
    LINES = {"a.txt": 17, "b.txt": 25, "c.txt": 9}
    PROMPT = (
        "Use child agents for this. Spawn one child to count the lines in a.txt and another to "
        "count the lines in b.txt; do not count those yourself. While they work, count the lines "
        "in c.txt yourself. Then collect both children's answers and reply with only the sum of "
        "all three counts, as a number."
    )
else:
    sys.exit(f"error: SCENARIO is fork or async, not {SCENARIO!r}")

results = []


def check(ok, what, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))


def alive(pid):
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def ledger(path):
    """Tokens and cost counted against the budget, and the count of holds nothing settled."""
    lines = []
    if os.path.exists(path):
        with open(path) as f:
            lines = [json.loads(l) for l in f if l.strip()]
    settled = {l["settles"] for l in lines if l.get("settles") is not None}
    open_holds = [i for i, l in enumerate(lines) if l.get("hold") and i not in settled]
    counted = [l for i, l in enumerate(lines) if not (l.get("hold") and i in settled)]
    return sum(l["tokens"] for l in counted), sum(l["cost"] for l in counted), len(open_holds)


def names(args, flag):
    """Whether args give flag (-P or -m), as getopt reads it: separate or attached."""
    return any(a == flag or (a.startswith(flag) and len(a) > 2) for a in args)


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def transcript(messages):
    """Print the shell commands and their results; return the commands and the results."""
    commands, outputs = [], []
    for m in messages:
        if m.get("role") == "assistant":
            for call in m.get("tool_calls") or []:
                try:
                    cmd = json.loads(call["function"]["arguments"]).get("command", "")
                except (KeyError, ValueError, AttributeError):
                    cmd = "?"
                commands.append(cmd)
                print(f"$ {cmd}")
            if (m.get("content") or "").strip():
                print(f"> {m['content'].strip()}")
        elif m.get("role") == "tool":
            body = (m.get("content") or "").rstrip()
            outputs.append(body)
            print("  " + body[:400].replace("\n", "\n  ") + (" ..." if len(body) > 400 else ""))
    return commands, outputs


def main():
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: no binary at {BIN}; run 'make' first")
    run = tempfile.mkdtemp(prefix="myra-recurse-live-")
    root = os.path.join(run, "root")
    state = os.path.join(run, "state")
    os.makedirs(root)
    for name, n in LINES.items():
        with open(os.path.join(root, name), "w") as f:
            f.writelines(f"line {i}\n" for i in range(1, n + 1))

    args = sys.argv[1:]
    if not names(args, "-P"):
        args = ["-P", "openrouter"] + ([] if names(args, "-m") else ["-m", MODEL]) + args
    cmd = [BIN, "-r", "--budget-tokens", str(BUDGET), "--json", *args, "-p", PROMPT]
    # Only the state directory is redirected, so the family lands in the run directory.
    env = dict(os.environ, XDG_STATE_HOME=state, MYRA_KEEP_FAMILY="1")
    print(f"run directory: {run}")
    print(f"binary: {BIN}")
    print(f"arguments: {' '.join(args)}")
    print(f"scenario: {SCENARIO}, budget: {BUDGET} tokens\n")
    try:
        out = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True,
                             timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        sys.exit(f"error: the root ran past {TIMEOUT}s; see {run}")
    with open(os.path.join(run, "root.log"), "w") as f:
        f.write(out.stdout + "\n--- stderr ---\n" + out.stderr)
    try:
        result = json.loads(out.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError):
        result = {}

    sessions = os.path.join(state, "myra", "sessions")
    session = load(os.path.join(sessions, f"{result.get('session_id')}.json")) or {}
    print("--- root transcript ---")
    commands, outputs = transcript(session.get("messages") or [])
    if result.get("error"):
        print(f"error: {result['error']}")
    if out.stderr.strip():
        print("--- root stderr ---\n" + out.stderr.rstrip())
    print()

    check(out.returncode == 0, "the root exits 0", f"exit {out.returncode}")
    check(bool(session), "the root's session was saved", f"session_id {result.get('session_id')}")
    families = os.path.join(state, "myra", "family")
    dirs = [os.path.join(families, d) for d in os.listdir(families)] if os.path.isdir(families) else []
    check(len(dirs) == 1, "one family was founded", f"{len(dirs)}")
    if len(dirs) != 1:
        return finish(run)
    family = dirs[0]
    registry = load(os.path.join(family, "registry.json")) or {"members": []}
    members = registry["members"]
    children = [m for m in members if m.get("parent") is not None]

    answer = (result.get("text") or "").strip()
    expected = str(sum(LINES.values()))
    check(expected in answer, f"the answer is {expected}", f"got {answer!r}")

    spawns = sum(c.count("-r spawn") for c in commands)
    waits = sum(c.count("-r wait") for c in commands)
    shutdowns = sum(c.count("-r shutdown") for c in commands)
    check(len(children) == 2, "two children were spawned", f"{len(children)} in the registry")
    check(spawns == len(children), "every spawn command is in the registry", f"{spawns} commands")
    check(waits >= len(children), "the root waited at least once per child", f"{waits} waits")
    check(shutdowns == 0, "the root never ran shutdown", f"{shutdowns} times")

    for m in children:
        path = os.path.join(family, f"{m['id']}.answer")
        got = open(path).read().strip() if os.path.exists(path) else None
        check(got is not None, f"child {m['name']} wrote an answer")
        print(f"      {m['name']} ({m['model']}, {m['permissions']}): {got!r}")
        check(os.path.exists(os.path.join(sessions, f"{m['id']}.json")),
              f"child {m['name']} saved its session under its id")
    check(all(m.get("ended_at") is not None for m in members), "every member is marked ended")
    check(not any(alive(m.get("pid")) for m in children), "no child process is left running")

    tokens, cost, holds = ledger(os.path.join(family, "ledger.jsonl"))
    check(holds == 0, "every hold was settled", f"{holds} open")
    check(tokens <= BUDGET, "spend is within the budget", f"{tokens} tokens")

    notes = sum(o.count("has finished;") for o in outputs)
    timeouts = sum(o.count("still working after") for o in outputs)
    polls = sum(c.count("-r agents") for c in commands)
    if SCENARIO == "async":
        first_wait = next((i for i, c in enumerate(commands) if "-r wait" in c), len(commands))
        own = [i for i, c in enumerate(commands) if "c.txt" in c and "-r spawn" not in c]
        check(bool(own) and own[0] < first_wait, "the root counted c.txt before its first wait",
              f"its own commands at {own}, first wait at {first_wait}")
        check(timeouts == 0 and polls == 0, "the root did not poll",
              f"{timeouts} timed-out waits, {polls} agents listings")
    print(f"\nfinished-child notes seen: {notes}, waits that timed out: {timeouts}, "
          f"agents listings: {polls}")
    print(f"spent: {tokens:.0f} tokens, ${cost:.4f}; root requests: {result.get('requests')}")
    print(f"logs: {family}/<id>.log, root output: {run}/root.log")
    return finish(run)


def finish(run):
    failed = results.count(False)
    print(f"\n{len(results) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
