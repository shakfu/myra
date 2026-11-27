# TODO

## Critical

## High

- [ ] Parallel agents: `myra_free` kills every agent's process groups, including a sibling's running `shell` call (`[exit 143]`), not only background jobs. The `jobs` list is static (`lib/tools.c:204`); a mutex now stops the heap corruption, not the cross-agent kill. Fix: a per-agent list, phase 3 of `docs/dev/agent-context.md`. Tests: `finishing_agent_leaves_siblings_running_call`, `finishing_agent_leaves_siblings_jobs` in `swarm/tests/test_swarm.py`.

- [ ] Parallel agents: a `shell` child inherits every fd open in the process, including sibling agents' curl sockets and `shell` pipes (`lib/tools.c:239`). A model-chosen command can then write to another agent's API connection. A leaked pipe also holds a sibling's call open until the child exits: `echo` took 0.56-0.68 s. Fix: `posix_spawn` with fds closed by default, phase 3. Test: `shell_child_inherits_no_sibling_fds`. Seen on macOS; Linux unchecked.

- [ ] Confirm on OpenRouter that context-full recovery works on Fable 5.1 and Opus 5.5. Recovery now strips `reasoning_details` when it blanks tool output, since Anthropic rejects thinking blocks after an edited turn for accounts created on or after 2026-08-31. Unverified: whether OpenRouter forwards the signatures, and whose account the check applies to (OpenRouter's, or a BYOK key's). Test: force a context-full retry mid tool loop with each model.

## Medium

- [ ] Verify the `reasoning_details` round-trip on OpenRouter. Run a multi-step tool task with a GPT-5.x and a Claude model; confirm replayed reasoning is accepted and used. A failure is a trigger in `docs/dev/native-providers.md`.

  - [x] Claude, accepted (2026-11-27, `anthropic/claude-opus-5.5`): a signed `reasoning.text` entry from a step that ran `shell` was replayed in the next request, which succeeded. Whether OpenRouter forwards it to Anthropic or drops it is not visible from the client. Adaptive thinking skips simple steps, so a test needs a task that makes the model think before a tool call.
  - [ ] GPT-5.x: not run.

- [ ] Run the suite and the load probe on Linux. Three fixes were verified on macOS (libedit, 8 cores) only, and each depends on terminal or signal behaviour that may differ:

  - [ ] `agent.py` Ctrl-C: SIGALRM must break `select()` with `EINTR` even with `SA_RESTART` set. signal(7) says Linux never restarts `select()`.

  - [ ] `myra` type-ahead: `poll()` must report only finished lines in canonical mode, and `TCSADRAIN` must keep an unfinished line readable once `ICANON` is off. Both are from memory.

  - [ ] The original Ctrl-C flake was seen on Linux with GNU readline (Python 3.12.3, 32 cores, 40 `yes` processes). Rerun under that load.

  - [ ]  The probe: with `Tty` from `tests/test_agent.py`, in one agent, repeat 300 times: type `q`, wait for its echo, send `\x03`, and count it lost if `\r\n` does not follow within 1 s. On macOS it lost 3 of 900 before the fix and 0 of 1500 after.

- [ ] Parallel agents: process-wide state that one agent's turn changes for all. Each item is in `docs/dev/agent-context.md`, "What breaks today", with its phase:

  - [ ] `myra_interrupted`: stopping one agent stops all (phase 1; test `stop_ends_only_its_agent`). `myra_ask` also clears it on entry, so a host's stop is undone by the next turn to start.

  - [ ] `myra_last_error`: about 1 in 200 agents failing at once reads another agent's error (phase 1; test `errors_stay_with_their_agent`).

  - [ ] cJSON's static `global_error` (`vendor/cjson/cJSON.c:92`) is written by every parse: a data race whenever two agents parse at once (phase 1).

  - [ ] The process cwd: two agents in different directories write to one of them (phase 2; test `each_agent_has_its_own_cwd`). ACP `session/new` and `myra-ui` change it with `chdir`.

  - [ ] Signals and curl: `lib/mcp.c:83-96` swaps the process's `SIGPIPE` disposition; `CURLOPT_NOSIGNAL` is unset; each agent runs `curl_global_init` and `curl_global_cleanup` (phases 3 and 4).

  - [ ] The MCP child calls `strerror` and `execvp` after `fork` (`lib/mcp.c:357-358`), which POSIX forbids in a threaded process (phase 3).

## Low

- [ ] `read` of a FIFO or a device that blocks (`/dev/tty`) hangs the turn, and Ctrl-C does not stop it. `open` on a FIFO blocks until a writer appears, and `fill_sink` (`lib/tools.c:70`) retries `read` on `EINTR` without checking `myra_interrupted`. Fix: open with `O_NONBLOCK`, refuse anything but a regular file (or poll with the interrupt check, as `tool_shell` does). Found by the live `myra-swarm` run, 2026-11-27.

- [ ] Arrow keys and other escape sequences typed during a turn reach the model as raw bytes in `myra`; `agent.py`'s libedit acts on them. A fix would hand a type-ahead line that contains ESC to linenoise instead of submitting it. Rare; wait for a report.

- [ ] An opt-in `make test-live`, skipped without `OPENROUTER_API_KEY`, would turn the two OpenRouter checks above into regression tests. It costs tokens per run.
