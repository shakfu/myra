"""myra-swarm PLAN.md: an agent per ## part, then one that combines their answers, in the
default --mode processes unless a test's mode mark says otherwise.

Each part's body holds a mock script as its last JSON line (mock.py). The objective's body
holds the combining agent's script: a part's prompt ends with its own body, so the part
runs its own script.
"""
import json
import signal
import subprocess
import time
import urllib.request

import pytest

MODE = "processes"


def script(**s):
    return json.dumps(s)


def plan(title, combine, parts, goal_text="The goal, in more words."):
    """A plan document: parts is a list of (title, script dict[, depends line])."""
    doc = f"# {title}\n\n{goal_text}\n\n{script(**combine)}\n"
    for name, s, *depends in parts:
        dep = f"depends: {depends[0]}\n" if depends else ""
        doc += f"\n## {name}\n\nContext for {name}.\n{dep}\n{script(**s)}\n"
    return doc


@pytest.fixture
def swarm(swarm_cmd, env, tmp_path):
    def run(doc, *args, timeout=60):
        path = tmp_path / "plan.md"
        path.write_text(doc)
        t0 = time.monotonic()
        r = subprocess.run([*swarm_cmd, *args, str(path)], env=env,
                           cwd=tmp_path, capture_output=True, text=True, timeout=timeout,
                           check=False)
        r.seconds = time.monotonic() - t0
        return r
    return run


def result(r):
    return json.loads(r.stdout)


def arrivals(server, says):
    """When each named script's first request reached the mock, by its "say"."""
    with urllib.request.urlopen(server.removesuffix("/v1") + "/_log") as f:
        log = json.load(f)
    return {say: min(t for t, s in log if s == say) for say in says}


def test_parts_run_in_parallel_then_combine(swarm, server):
    doc = plan("Why is the build slow?", {"say": "final", "echo": True},
               [(f"Part {i}", {"say": f"answer{i}", "delay": 1, "echo": True}) for i in range(3)])
    r = swarm(doc, "--json")
    assert r.returncode == 0, r.stderr
    starts = arrivals(server, [f"answer{i}" for i in range(3)]).values()
    # One at a time, each would arrive after the previous one's 1 s reply.
    assert max(starts) - min(starts) < 1, f"parts started {max(starts) - min(starts):.2f} s apart"
    out = result(r)
    assert out["ok"] and out["objective"] == "Why is the build slow?"
    for i, part in enumerate(out["parts"]):
        assert part["ok"] and part["title"] == f"Part {i}"
        prompt = part["answer"]
        assert prompt.startswith(f"answer{i}\n")
        assert "# Objective: Why is the build slow?\n\nThe goal, in more words." in prompt
        assert f"## Your part: Part {i}\n\nContext for Part {i}." in prompt
        assert "Keep the answer under 400 words" in prompt
        others = prompt.split("## Other parts, handled by other agents")[1]
        assert f"- Part {i}\n" not in others
        assert all(f"- Part {j}\n" in others for j in range(3) if j != i)
    combined = out["answer"]
    assert combined.startswith("final\n")
    for i in range(3):
        assert f'<answer part="{i + 1}">\nPart: Part {i}\n\nanswer{i}' in combined


def test_markdown_report(swarm):
    doc = plan("Objective", {"say": "the combined answer"},
               [("Works", {"say": "good"}), ("Breaks", {"say": "bad", "status": 400})])
    r = swarm(doc)
    assert r.returncode == 1  # a part failed
    assert r.stdout.startswith("# Objective\n\nthe combined answer\n\n---\n\n## Works\n\ngood\n\n")
    assert "## Breaks\n\n*Failed: api error (http 400): " in r.stdout
    assert "fail-bad" in r.stdout
    assert "[myra-swarm] 2 parts of: Objective\n" in r.stderr
    assert "/2 failed in " in r.stderr and "/2 done in " in r.stderr
    # The failed request reports no usage, so it is not counted.
    assert "[myra-swarm] 1 of 2 parts done; 2 requests" in r.stderr


def test_failed_part_reaches_the_combiner(swarm):
    doc = plan("Objective", {"say": "final", "echo": True},
               [("Fine", {"say": "fine"}), ("Broken", {"say": "broken", "status": 400})])
    out = result(swarm(doc, "--json"))
    assert not out["ok"] and out["combine"]["ok"]
    assert [p["ok"] for p in out["parts"]] == [True, False]
    assert "This part failed: api error (http 400)" in out["answer"]
    assert "fail-broken" in out["answer"]


def test_every_part_failing_skips_the_combiner(swarm):
    doc = plan("Objective", {"say": "final"},
               [("A", {"say": "a", "status": 400}), ("B", {"say": "b", "status": 400})])
    r = swarm(doc, "--json")
    out = result(r)
    assert r.returncode == 1 and not out["ok"]
    assert out["error"] == "every part failed" and out["combine"]["requests"] == 0


@pytest.mark.mode("threads")
def test_shell_calls_survive_a_sibling_finishing(swarm):
    # Threads mode frees agents only after every part has finished (swarm.c, worker); this
    # is that workaround. In processes mode the parts share no shell jobs to kill.
    doc = plan("Objective", {"say": "final"}, [
        ("Slow", {"say": "slow", "calls": [["shell", {"command": "sleep 1; echo finished"}]]}),
        ("Quick", {"say": "quick", "delay": 0.3}),
    ])
    out = result(swarm(doc, "--json"))
    assert out["ok"], out
    assert "finished\n[exit 0]" in out["parts"][0]["answer"]


def test_jobs_limit_runs_parts_in_turn(swarm):
    doc = plan("Objective", {"say": "final"},
               [(f"P{i}", {"say": f"p{i}", "delay": 0.3}) for i in range(3)])
    r = swarm(doc, "-j", "1", "--json")
    assert r.returncode == 0 and result(r)["ok"]
    assert r.seconds >= 0.9, f"three 0.3 s parts one at a time took {r.seconds:.1f} s"


def test_ctrl_c_stops_parts_and_skips_the_combiner(swarm_cmd, env, tmp_path):
    doc = plan("Objective", {"say": "final"},
               [(f"P{i}", {"say": "p", "calls": [["shell", {"command": "sleep 10"}]]})
                for i in range(2)])
    (tmp_path / "plan.md").write_text(doc)
    p = subprocess.Popen([*swarm_cmd, "--json", "plan.md"],
                         env=env, cwd=tmp_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True)
    time.sleep(0.5)
    p.send_signal(signal.SIGINT)
    out, _ = p.communicate(timeout=10)
    assert p.returncode == 130
    r = json.loads(out)
    assert r["error"] == "stopped" and r["combine"]["requests"] == 0
    assert not any(part["ok"] for part in r["parts"])


def test_headings_in_code_blocks_and_closing_hashes(swarm):
    doc = ("# Objective #\n\n" + script(say="final") + "\n\n## Only part ##\n\n"
           "```\n# not the objective\n## not a part\n```\n\n### Detail\n\n"
           + script(say="only", echo=True) + "\n")
    out = result(swarm(doc, "--json"))
    assert out["objective"] == "Objective"
    assert [p["title"] for p in out["parts"]] == ["Only part"]
    assert "```\n# not the objective\n## not a part\n```\n\n### Detail" in out["parts"][0]["answer"]


def test_plan_from_stdin(swarm_cmd, env, tmp_path):
    doc = plan("Objective", {"say": "final"}, [("A", {"say": "a"})])
    r = subprocess.run([*swarm_cmd, "--json", "-"], input=doc,
                       env=env, cwd=tmp_path, capture_output=True, text=True, timeout=30,
                       check=False)
    assert r.returncode == 0 and json.loads(r.stdout)["answer"] == "final"


@pytest.mark.parametrize("doc, why", [
    ("Just text.\n", "line 1: text before the # heading"),
    ("## Part\n", "line 1: a ## heading before the # heading"),
    ("# One\n## A\n# Two\n", "line 3: a second # heading; a plan has one objective"),
    ("# Only an objective\n\nNo parts.\n", "no ## headings: a plan needs at least one part"),
    ("\n\n", "no # heading: a plan needs one objective"),
    ("# Objective\n##\n", "line 2: a heading without a title"),
])
def test_bad_plans_are_refused(swarm, doc, why):
    r = swarm(doc)
    assert r.returncode == 2 and r.stdout == ""
    assert r.stderr.endswith(f"plan.md: {why}\n"), r.stderr


@pytest.mark.parametrize("args, phrase", [
    ([], "Keep the answer under 400 words"),
    (["--words", "50"], "Keep the answer under 50 words"),
    (["--words", "0"], None),
])
def test_parts_are_asked_for_short_answers(swarm, args, phrase):
    doc = plan("Objective", {"say": "final"}, [("A", {"say": "a", "echo": True})])
    prompt = result(swarm(doc, "--json", *args))["parts"][0]["answer"]
    if phrase:
        assert phrase in prompt
    else:
        assert "Keep the answer under" not in prompt


def test_depends_orders_parts_and_passes_answers(swarm, server):
    # C depends on A and B; B depends on A. D depends on nothing and runs beside A.
    doc = plan("Objective", {"say": "final"}, [
        ("C", {"say": "answer-c", "echo": True}, "A, B"),
        ("A", {"say": "answer-a", "delay": 1}),
        ("B", {"say": "answer-b", "echo": True}, "A"),
        ("D", {"say": "answer-d", "delay": 1}),
    ])
    r = swarm(doc, "--json")
    out = result(r)
    assert out["ok"], out
    start = arrivals(server, ["answer-a", "answer-d"])
    assert abs(start["answer-a"] - start["answer-d"]) < 1, "D did not run beside A"
    c, _, b, d = out["parts"]
    assert c["depends"] == ["A", "B"] and b["depends"] == ["A"] and d["depends"] == []
    assert "depends:" not in c["answer"]  # the line is not context
    assert '<answer part="2">\nPart: A\n\nanswer-a\n</answer>' in b["answer"]
    for part, answer in [(2, "answer-a"), (3, "answer-b")]:
        assert f'<answer part="{part}">\nPart: {"AB"[part - 2]}\n\n{answer}' in c["answer"]
    order = [line.split(": ")[-1] for line in r.stderr.splitlines() if " done in " in line]
    assert order.index("A") < order.index("B") < order.index("C"), r.stderr


def test_depends_waits_even_with_one_agent_at_a_time(swarm):
    # The first part listed waits on the last: -j 1 must not deadlock.
    doc = plan("Objective", {"say": "final"},
               [("First", {"say": "first"}, "Last"), ("Last", {"say": "last"})])
    r = swarm(doc, "-j", "1", "--json", timeout=20)
    assert r.returncode == 0 and result(r)["ok"]


def test_failed_dependency_skips_its_dependents(swarm):
    doc = plan("Objective", {"say": "final", "echo": True}, [
        ("Base", {"say": "base", "status": 400}),
        ("Built", {"say": "built"}, "Base"),
        ("On top", {"say": "top"}, "Built"),
        ("Apart", {"say": "apart"}),
    ])
    r = swarm(doc, "--json")
    out = result(r)
    assert r.returncode == 1
    base, built, top, apart = out["parts"]
    assert not base["ok"] and apart["ok"]
    assert built["error"] == 'not started: it depends on "Base", which failed'
    assert top["error"] == 'not started: it depends on "Built", which failed'
    assert built["requests"] == 0 and top["requests"] == 0
    assert 'This part failed: not started: it depends on "Base", which failed' in out["answer"]


@pytest.mark.parametrize("doc, why", [
    ("# O\n## A\ndepends: B\n", 'line 3: depends: no part is titled "B"'),
    ("# O\n## A\ndepends: A\n", 'line 3: "A" depends on itself'),
    ("# O\n## A\ndepends: B\n## B\ndepends: C\n## C\nDepends: A\n",
     'parts depend on each other: "A" -> "B" -> "C" -> "A"'),
    ("# O\ndepends: A\n## A\n", "line 2: depends: under the # heading; only parts have it"),
    ("# O\n## A\ndepends: , \n", "line 3: depends: names no part"),
    ("# O\n## A\n## A\n", 'two parts are titled "A"'),
])
def test_bad_dependencies_are_refused(swarm, doc, why):
    r = swarm(doc)
    assert r.returncode == 2 and r.stdout == ""
    assert r.stderr.endswith(f"plan.md: {why}\n"), r.stderr


def test_depends_inside_a_code_block_is_text(swarm):
    doc = ("# O\n" + script(say="final") + "\n## A\n```\ndepends: Nothing\n```\n"
           + script(say="a", echo=True) + "\n")
    out = result(swarm(doc, "--json"))
    assert out["ok"] and "```\ndepends: Nothing\n```" in out["parts"][0]["answer"]


def test_budget_makes_every_agent_a_member_of_one_family(swarm, tmp_path):
    doc = plan("Objective", {"say": "final", "whoami": True},
               [("A", {"say": "a", "whoami": True}), ("B", {"say": "b", "whoami": True})])
    out = result(swarm(doc, "--json", "--budget-tokens", "1000000"))
    assert out["ok"], out
    assert [p["answer"].splitlines()[-1] for p in out["parts"]] == ["agent part-1", "agent part-2"]
    assert out["answer"].splitlines()[-1] == "agent combine"
    assert not list((tmp_path / "state/myra/family").glob("*"))  # the host removed it


def test_without_a_budget_agents_are_not_members(swarm):
    out = result(swarm(plan("Objective", {"say": "final"}, [("A", {"say": "a", "whoami": True})]),
                       "--json"))
    assert out["parts"][0]["answer"].splitlines()[-1] == "agent -"


def test_a_spent_budget_fails_every_agent(swarm):
    doc = plan("Objective", {"say": "final"}, [("A", {"say": "a"}), ("B", {"say": "b"})])
    r = swarm(doc, "--json", "--budget-tokens", "5000")  # less than one request's floor
    out = result(r)
    assert r.returncode == 1 and out["error"] == "every part failed"
    assert all("the family's token budget of 5000 is spent" in p["error"] for p in out["parts"])
    assert all(p["requests"] == 0 for p in out["parts"])


@pytest.mark.parametrize("args, why", [
    (["--mode", "threads", "--budget-tokens", "9"], "a budget needs --mode processes"),
    (["--budget-cost", "1"], "--budget-cost needs --budget-tokens too"),
])
def test_budget_option_errors(swarm, args, why):
    r = swarm(plan("Objective", {"say": "final"}, [("A", {"say": "a"})]), *args)
    assert r.returncode == 2 and why in r.stderr


def test_a_dead_host_fails_every_agent(swarm, tmp_path):
    # It closes its stdin before it prints the id, so the first add hits a closed pipe.
    fake = tmp_path / "fake-myra"
    fake.write_text('#!/bin/sh\nexec 0<&-\necho 1-1\n')
    fake.chmod(0o755)
    r = swarm(plan("Objective", {"say": "final"}, [("A", {"say": "a"})]),
              "--json", "--budget-tokens", "9", "--myra", str(fake))
    out = result(r)
    assert r.returncode == 1 and out["error"] == "every part failed", (r.returncode, r.stderr)
    assert out["parts"][0]["error"] == "cannot join the family: the host exited"
