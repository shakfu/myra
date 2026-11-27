# Per-agent context: plan

Proposed 2026-11-27, at commit `49691ed` plus the uncommitted `lib/`/`cli/`/`include/` split. Not built. Names marked *proposed* do not exist. `myra-swarm` (`swarm/`) and its tests check the failures below; see [Evidence](#evidence).

## Goal

Let one process run several `myra_agent`s at once, each on its own thread and in its own working directory. The target is any host with parallel agents: a GUI with a tab per agent, or `myra-swarm`, which runs one agent per task and has no UI.

The alternative is one `myra --acp` process per session. It gets process isolation for free and costs IPC, plus possible drift between ACP and the C API.

## What breaks today

With two agents in one process:

| State | Where | Failure |
|-|-|-|
| `myra_interrupted` | `lib/agent.c:20`, 13 reads in `lib/` | Stopping one agent stops every agent |
| `myra_poll` | `lib/agent.c:21` | One hook for all agents; ACP and `family.c` each install their own |
| `myra_note_hook` | `lib/agent.c:22` | No user data. Each agent notes only on its own thread, so a host can use a `_Thread_local` to tell agents apart, as `myra-swarm` does. `ui/worker.c:10` keeps a static `the_app` instead |
| `myra_last_error` | `lib/agent.c:23` | Agent B's error overwrites A's before A's thread reads it: 14 of 2,560 agents failing at once |
| Process cwd | `myra__absolute` (`lib/text.c:42`), `myra__outside_cwd` (`lib/agent.c:46`), `myra_set_cwd`, session load/save (`lib/state.c:106`, `:129`), `shell` and MCP children | One cwd for all agents. `chdir` in ACP `session/new` (`cli/acp.c:343`) and `myra-ui` (`ui/main.c:55`) moves every agent |
| `jobs` | `lib/tools.c:204` | `myra_free` on one agent SIGTERMs every agent's process groups: background jobs, and also `shell` calls still running (`[exit 143]`). A mutex guards the list since 2026-11-27; before it, `add_job` could corrupt the heap |
| cJSON's `global_error` | `vendor/cjson/cJSON.c:92`, written by every parse (`:1148`) | A data race whenever two agents parse at once. ThreadSanitizer reports it on every run |
| MCP child calls `strerror` and `execvp` | `lib/mcp.c:357-358` | Neither is on POSIX's async-signal-safe list. After `fork` in a threaded process, the child may only call functions on that list ([fork](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html)) |
| `sigaction(SIGPIPE)` save and restore | `lib/mcp.c:83-96` | Swaps a process-wide disposition, so it races with other threads and with the host |
| curl signals | no `CURLOPT_NOSIGNAL` | libcurl requires `CURLOPT_NOSIGNAL` in threaded programs ([threadsafe](https://curl.se/libcurl/c/threadsafe.html)) |
| `curl_global_init`/`cleanup` per agent | `lib/agent.c:216`, `:242` | Thread-safe only in libcurl 7.84+ built with `CURL_VERSION_THREADSAFE` ([curl_global_init](https://curl.se/libcurl/c/curl_global_init.html)). Local is 8.7.1. That cleanup is covered too is from memory |
| `getenv` per request | `request` (`lib/api.c:308`, `:318`) | Races with any `setenv` in the host |

`myra_terminated` stays global. A termination signal ends the whole process.

## Design

### State moves into `myra_agent`

```c
struct myra_agent {
    ...
    atomic_int interrupted;   /* proposed; replaces myra_interrupted */
    char *cwd;                /* proposed; absolute, owned */
    char last_error[1024];    /* proposed; replaces myra_last_error */
    pid_t *jobs;              /* proposed; replaces the static list */
    size_t njobs, jobs_cap;
    char *base_url;           /* proposed; read from the env once, in myra_init */
    long retry_delay_ms;      /* proposed; likewise */
};

typedef struct {
    ...
    void (*note)(void *ud, myra_style style, const char *text); /* proposed; replaces myra_note_hook */
    void (*poll)(void *ud);                                     /* proposed; replaces myra_poll */
} myra_io;

void myra_note(myra_agent *a, myra_style style, const char *fmt, ...); /* a NULL: stderr */
int  myra_set_cwd(myra_agent *a, const char *dir); /* proposed signature; NULL: getcwd() */
```

Decisions:

| Question | Decision | Over |
|-|-|-|
| Where state lives | Fields on `myra_agent` | A `myra_ctx` shared by agents. Nothing found needs sharing except curl init, which `pthread_once` covers |
| Agentless functions that report errors | Take the agent: `myra_session_load(a, id)`, `myra_mcp_load(a, path)`. `myra_store_state` returns -1, and the caller reports it | Out-parameters such as `char **why` on each. Notes already carry these messages |
| A signal handler stopping a turn | The CLI keeps a static `myra_agent *` and sets `->interrupted` | A global flag. A lock-free atomic is safe to write from a handler |
| Paths | Relative tool paths resolve against `a->cwd`. `outside_cwd` compares against `realpath(a->cwd)` | Per-thread `chdir`. macOS has `pthread_chdir_np`, but it is private, and Linux has no equivalent |
| Starting children | `posix_spawn` with the child's cwd, process group, fds and default `SIGPIPE` set by attributes | `fork` plus fixes. See below |
| `SIGPIPE` | The host must ignore it, as `myra.h` will state. The CLI sets `SIG_IGN`, and children get `SIG_DFL` back through `POSIX_SPAWN_SETSIGDEF` | Per-thread `pthread_sigmask` plus `sigtimedwait`. That needs more code and still does not cover curl's TLS writes under `CURLOPT_NOSIGNAL` |
| curl global init | `pthread_once` in `myra_init`; no cleanup | Per-agent init and cleanup, which needs a libcurl feature check. Or a new `myra_global_init` the host must call |

### Child processes: `posix_spawn`

`posix_spawn` replaces `fork` in `tool_shell` and MCP `start`:

- **Working directory:** `posix_spawn_file_actions_addchdir_np(a->cwd)`. Needs glibc 2.29+ or macOS 10.15+.
- **Process group:** `POSIX_SPAWN_SETPGROUP` with group 0 replaces `setpgid(0, 0)`.
- **stdin and pipes:** `addopen(0, "/dev/null")` and `adddup2` do what the current child code does.
- **Signals:** `POSIX_SPAWN_SETSIGDEF` resets `SIGPIPE` to its default. This matters once the host ignores `SIGPIPE`: an exec'd child inherits an ignored disposition, and `yes | head` would then print EPIPE errors.
- **Inherited fds:**
  - macOS: `POSIX_SPAWN_CLOEXEC_DEFAULT` closes every fd not named in a file action.
  - Linux: create the pipes with `pipe2(O_CLOEXEC)` and add `posix_spawn_file_actions_addclosefrom_np(3)` (glibc 2.34+).
- **MCP exec errors:** `posix_spawnp` returns the exec error to the parent (glibc 2.24+ and macOS; inference from the man pages, verify both). The parent then writes the `cannot run` note itself. That also reaches a GUI with no stderr.

This removes every fork-in-a-threaded-process rule from the library. A host that forks by itself can still leak its fds into our children on Linux before glibc 2.34; `myra.h` will say so.

### Thread contract for `myra.h`

- An agent is used by one thread at a time. The exception is `interrupted`: any thread or a signal handler may set it.
- Hooks run on the thread that called `myra_ask`.
- The process ignores `SIGPIPE`.
- Nothing calls `setenv` while agents run. `XDG_STATE_HOME` and `HOME` are still read on every state access, since a user's state directory is per-process.
- `myra_escape`, `myra_md_*` and `myra_utf8` take explicit state and are safe on any thread.

## Phases

Each phase passes `make test`, `make asan` and `make test-ui` alone.

1. **Interrupt, errors, notes, poll.**
   - Add `interrupted` and `last_error`, plus `io.note` and `io.poll`.
   - Make cJSON's `global_error` `_Thread_local`: one line in `vendor/cjson/cJSON.c`, marked `myra:` as in linenoise. The alternative, never calling `cJSON_GetErrorPtr`, still races on the write.
   - `myra_note` gains the agent argument: 38 call sites in `lib/`, 59 in `cli/`.
   - Remove the four globals. Update `cli/main.c`, `cli/acp.c`, `cli/family.c`, `ui/worker.c` and `ui/view.c`.
   - Add `make tsan` (`-fsanitize=thread`, a new value next to `MYRA_SANITIZE`). ASan cannot see data races. TSan misses the `myra_last_error` race, since the writes happen inside libc's `vsnprintf`.
2. **Working directory.**
   - Add `a->cwd` and `myra_set_cwd(a, dir)`.
   - Resolve tool paths against it, and check sessions against it.
   - ACP `session/new` and `myra-ui` stop calling `chdir`. The CLI's `-C` keeps its `chdir`, since one agent runs per process.
   - Until phase 3, `shell` and MCP children still use the process cwd, so the child does a `chdir(a->cwd)` (async-signal-safe).
3. **Children.**
   - Switch to `posix_spawn` as above, with a per-agent `jobs` list.
   - Since 2026-11-27, `shell` and MCP children close fds 3 and up after `fork` (`close_from_3` in `lib/util.h`), so they inherit no sibling's sockets or pipes. `posix_spawn` replaces it.
   - Drop the `SIGPIPE` save and restore in `mcp.c`. The CLI sets `SIG_IGN`.
4. **curl.** Set `CURLOPT_NOSIGNAL`, run init through `pthread_once`, and read `base_url` and `retry_delay_ms` once in `myra_init`.

Phase 1 alone fixes the shared stop button and lets `myra-ui` drop `the_app`. Phase 3 is what makes parallel `shell` calls usable: until then, any agent finishing can kill a sibling's running command.

`scripts/agent.py` does not change: it mirrors CLI behaviour, and the CLI's behaviour stays the same. The e2e suite is the regression check.

## Evidence

`myra-swarm --tasks` reads one JSON task per line and runs each as an agent: on its own thread with `--mode threads`, or as a child `myra` with `--mode processes`. It uses only `include/myra/`. Its suite, `swarm/tests/test_swarm.py`, drives it against a scripted server (`swarm/tests/mock.py`) and runs as ctest `swarm`, every case in both modes. Each test failing today in threads mode is marked `threads_bug` with its phase, which makes it `xfail` there only. A strict one fails on every run, so a fix turns it into XPASS, which fails the suite until the mark is removed. In processes mode every one of them passes, so the failures are process-wide state, not the tests.

| Test | Today | Phase | xfail |
|-|-|-|-|
| `parallel_turns_stay_apart`: 16 agents write, read and edit | passes | - | - |
| `stop_ends_only_its_agent` | fails: the sibling is interrupted too | 1 | strict |
| `errors_stay_with_their_agent`: 5 x 128 agents fail at once | fails at 1 agent in 200, so in an estimated 97% of runs | 1 | not strict |
| `each_agent_has_its_own_cwd` | fails: both files land in one directory | 2 | strict |
| `finishing_agent_leaves_siblings_running_call` | fails: `[exit 143]` | 3 | strict |
| `finishing_agent_leaves_siblings_jobs` | fails: the background job is killed | 3 | strict |
| `shell_child_inherits_no_sibling_fds`: a sibling's `shell` runs during the probe | passes. The probe duplicates fds 3-63 and compares with a bare `/bin/sh` run, since bash shows a descriptor of its own; an earlier `ls /dev/fd` version failed on macOS even without a leak | 3 | - |

Strict tests were stable in 8 of 8 runs on macOS, 2026-11-27. The baseline passes only because it avoids `shell`. With `shell`, a finishing agent killed a sibling's `cat` in 2 of 3 runs.

Plan mode (`myra-swarm PLAN.md`) works around the `jobs` row: it frees no agent until every part has finished. A host can do this only when its agents end together. A GUI that closes one tab cannot. Ctrl-C works there because stopping every agent is what it wants. The app keeps its own stop flag, since `myra_ask` clears `myra_interrupted` when the next part starts.

ThreadSanitizer, on `myra-swarm` running 8 agents that each write, read and `shell`, reported 28 races in `add_job`, `myra_kill_jobs` and cJSON's parse before the `jobs` mutex. Build it with `cmake -S . -B build-tsan -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread` until `make tsan` exists.

## Tests still to add

Unit tests, in `tests/unit.c`, for what `myra-swarm` cannot reach:

- Each note reaches `io.note` with its own agent's `ud`.
- `outside_cwd` refuses B's directory for A under `MYRA_AUTO`.
- An MCP server whose command does not exist produces the `cannot run` note through `io.note`, not stderr.
- `yes | head -1` in `shell` exits 0 with no EPIPE text while the process ignores `SIGPIPE`.

`make tsan` runs these and the `swarm` suite.

## Non-goals

- **Out of memory.** `exit(1)` on allocation failure stays (`util.h` and most of `lib/`). Returning errors instead touches every `buf_add` caller. It needs its own plan.
- **Terminal output.** Done separately: `libmyra-term` (`term/`) holds it, and `myralib` prints nothing.
- **Pluggable tools.** Done separately: `myra_add_tool`.

## Open questions

- **Interrupt lost before a turn.** `myra_ask` clears `interrupted` on entry. A stop that arrives between the host starting the worker and `myra_ask`'s first line is lost. Clearing it on return instead would carry a stop forward into the next turn. Which is right for a GUI?
- **Per-agent credentials.** `myra_init` fails when `key_env` is unset (`lib/agent.c:198`). A host that keeps keys in a keychain would need an API key passed to `myra_init`. Is that needed now?
- **`SIGPIPE` policy.** Is requiring the host to ignore it acceptable? CUI, the GUI toolkit `myra-ui` uses, may already set a handler; not checked.
