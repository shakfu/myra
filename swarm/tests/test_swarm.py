"""Agents sharing one process, through myra-swarm --tasks --mode threads.

Each test is one row of the "What breaks today" table in docs/dev/agent-context.md. A test
that fails before the plan's phase N is marked xfail with that phase. strict: it fails
every run, so a fix turns it into XPASS, which fails the suite until the mark is removed.
"""
import json
import subprocess

import pytest

MODE = "threads"  # the library's process-wide state is what these tests are about


@pytest.fixture
def swarm(swarm_cmd, env, tmp_path):
    def run(tasks, timeout=60):
        lines = "".join(json.dumps(t) + "\n" for t in tasks)
        r = subprocess.run([*swarm_cmd, "--tasks"], input=lines,
                           env=env, cwd=tmp_path, capture_output=True, text=True,
                           timeout=timeout, check=False)
        assert r.stderr == ""
        out = [json.loads(x) for x in r.stdout.splitlines()]
        assert len(out) == len(tasks)
        return out
    return run


def prompt(**script):
    return json.dumps(script)


# No shell: a finishing agent kills its siblings' running calls (the test below).
def test_parallel_turns_stay_apart(swarm, tmp_path):
    tasks = [{"prompt": prompt(say=f"agent{i}", calls=[
        ["write", {"path": f"f{i}.txt", "content": f"body{i}\n"}],
        ["read", {"path": f"f{i}.txt"}],
        ["edit", {"path": f"f{i}.txt", "old_string": "body", "new_string": "edited"}],
        ["read", {"path": f"f{i}.txt"}]])} for i in range(16)]
    for i, r in enumerate(swarm(tasks)):
        assert r["ok"], r
        assert r["text"] == f"agent{i}\nwrote f{i}.txt\nbody{i}\n\nedited f{i}.txt\nedited{i}\n", r
        assert (tmp_path / f"f{i}.txt").read_text() == f"edited{i}\n"


@pytest.mark.xfail(strict=True, reason="phase 3: myra_free kills every agent's running calls")
def test_finishing_agent_leaves_siblings_running_call(swarm):
    slow, quick = swarm([
        {"prompt": prompt(say="slow", calls=[["shell", {"command": "sleep 1; echo finished"}]])},
        {"prompt": prompt(say="quick", delay=0.3)}, # ends while slow's sleep runs
    ])
    assert quick["ok"]
    assert slow["ok"] and "finished\n[exit 0]" in slow["text"], slow


@pytest.mark.xfail(strict=True, reason="phase 1: myra_interrupted is process-wide")
def test_stop_ends_only_its_agent(swarm):
    stopped, other = swarm([
        {"prompt": prompt(say="a", calls=[["shell", {"command": "sleep 3"}]]), "stop_after": 0.5},
        {"prompt": prompt(say="b", calls=[["shell", {"command": "sleep 1; echo survived"}]])},
    ])
    assert stopped["stopped"] and "interrupted" in stopped["text"] + stopped["notes"]
    assert other["ok"] and "survived" in other["text"], other


# Racy: about 1 agent in 200 failing at once reads another's error (macOS, 2026-11-27).
@pytest.mark.xfail(strict=False, reason="phase 1: myra_last_error is process-wide")
def test_errors_stay_with_their_agent(swarm):
    n, wrong = 128, 0
    for _ in range(5):
        out = swarm([{"prompt": prompt(say=f"m{i:03d}", status=400)} for i in range(n)])
        for i, r in enumerate(out):
            assert not r["ok"] and f"fail-m{i:03d}" in r["error"], r
        wrong += sum(f"fail-m{i:03d}" not in r["last_error"] for i, r in enumerate(out))
    assert not wrong, f"{wrong} of {5 * n} read another agent's myra_last_error"


@pytest.mark.xfail(strict=True, reason="phase 2: tools resolve paths against the process cwd")
def test_each_agent_has_its_own_cwd(swarm, tmp_path):
    dirs = [tmp_path / "a", tmp_path / "b"]
    for d in dirs:
        d.mkdir()
    swarm([{"prompt": prompt(say=d.name, delay=0.3,
                             calls=[["write", {"path": "where.txt", "content": d.name}]]),
            "cwd": str(d)} for d in dirs])
    found = {d.name: (d / "where.txt").read_text() if (d / "where.txt").exists() else None
             for d in dirs}
    assert found == {"a": "a", "b": "b"}


@pytest.mark.xfail(strict=True, reason="phase 3: myra_free kills every agent's jobs")
def test_finishing_agent_leaves_siblings_jobs(swarm, tmp_path):
    long, short = swarm([
        {"prompt": prompt(say="long", calls=[
            ["shell", {"command": "(sleep 0.5; touch alive) > /dev/null 2>&1 &"}],
            ["shell", {"command": "sleep 1; ls alive"}]])},
        {"prompt": prompt(say="short", calls=[["shell", {"command": "true"}]])},
    ])
    assert short["ok"]
    assert long["ok"] and (tmp_path / "alive").exists(), long


@pytest.mark.xfail(strict=True, reason="phase 3: shell children inherit every open fd")
def test_shell_child_inherits_no_sibling_fds(swarm):
    # b's shell pipe is open while a's probe runs; libcurl 8.18 sets FD_CLOEXEC on its
    # sockets, so only the pipe leaks there. b ends first: a's myra_free would kill its sleep.
    # The probe opens nothing itself; bash still shows a descriptor of its own, so a bare sh
    # run sets the baseline.
    probe = ("exec 2>/dev/null; fd=3; while [ $fd -lt 64 ]; do (exec <&$fd) && echo open $fd; "
             "fd=$((fd+1)); done")
    baseline = subprocess.run(["/bin/sh", "-c", probe], capture_output=True, text=True,
                              check=False).stdout.split("\n")
    a, b = swarm([
        {"prompt": prompt(say="a", delay=1.0, calls=[["shell", {"command": probe}]])},
        {"prompt": prompt(say="b", calls=[["shell", {"command": "sleep 1.5"}]])},
    ])
    assert a["ok"] and b["ok"], (a, b)
    opened = [line for line in a["text"].split("\n") if line.startswith("open ")]
    assert opened == [line for line in baseline if line], f"open in a's shell: {opened}"
