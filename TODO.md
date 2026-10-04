# TODO

## Critical

## High

- Confirm on OpenRouter that context-full recovery works on Fable 5.1 and Opus 5.5. Recovery now strips `reasoning_details` when it blanks tool output, since Anthropic rejects thinking blocks after an edited turn for accounts created on or after 2026-08-31. Unverified: whether OpenRouter forwards the signatures, and whose account the check applies to (OpenRouter's, or a BYOK key's). Test: force a context-full retry mid tool loop with each model.

## Medium

- Verify the `reasoning_details` round-trip on OpenRouter. Run a multi-step tool task with a GPT-5.x and a Claude model; confirm replayed reasoning is accepted and used. A failure is a trigger in `docs/dev/native-providers.md`.

- Run the suite and the load probe on Linux. Three fixes were verified on macOS (libedit, 8 cores) only, and each depends on terminal or signal behaviour that may differ:
  - `agent.py` Ctrl-C: SIGALRM must break `select()` with `EINTR` even with `SA_RESTART` set. signal(7) says Linux never restarts `select()`.
  - `myra` type-ahead: `poll()` must report only finished lines in canonical mode, and `TCSADRAIN` must keep an unfinished line readable once `ICANON` is off. Both are from memory.
  - The original Ctrl-C flake was seen on Linux with GNU readline (Python 3.12.3, 32 cores, 40 `yes` processes). Rerun under that load.

  The probe: with `Tty` from `tests/test_agent.py`, in one agent, repeat 300 times: type `q`, wait for its echo, send `\x03`, and count it lost if `\r\n` does not follow within 1 s. On macOS it lost 3 of 900 before the fix and 0 of 1500 after.

## Low

- Arrow keys and other escape sequences typed during a turn reach the model as raw bytes in `myra`; `agent.py`'s libedit acts on them. A fix would hand a type-ahead line that contains ESC to linenoise instead of submitting it. Rare; wait for a report.

- An opt-in `make test-live`, skipped without `OPENROUTER_API_KEY`, would turn the two OpenRouter checks above into regression tests. It costs tokens per run.
