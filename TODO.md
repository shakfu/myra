# TODO

## Critical

## High

- Check whether context-full recovery breaks on Fable 5.1 and Opus 5.5 via OpenRouter. `drop_old_tool_output` edits earlier tool results; those models reject edited history for accounts created on or after 2026-08-31 if thinking blocks are forwarded. Unverified.

## Medium

- Verify the `reasoning_details` round-trip on OpenRouter. Run a multi-step tool task with a GPT-5.x and a Claude model; confirm replayed reasoning is accepted and used. A failure is a trigger in `docs/dev/native-providers.md`.

- `test_terminal_line_editing_history_and_ctrl_c` flakes against `scripts/agent.py` under CPU load. The Ctrl-C after `abc` produces no output for 10 s. Failure counts on 2026-09-24, Python 3.12.3, 40 `yes` processes on 32 cores:

  | Agent | Loaded | Idle |
  |-|-|-|
  | `agent.py` | 10/40 | 0/60 |
  | `myra` | 0/40 | not run |

  Hypothesis: CPython's `readline` loses the SIGINT. `readline_until_enter_or_signal` (`Modules/readline.c`) blocks in `select()` with no timeout unless `PyOS_InputHook` is set. It checks signals only when `select()` fails with `EINTR`. A SIGINT that arrives while `rl_callback_read_char` runs sets the flag, but the next `select()` still blocks until a key arrives. If so, a user sees Ctrl-C take effect only on the next keypress. Unverified; the `readline.c` behaviour is from memory.

  Test: under the same load, send `\x03`. If nothing arrives within 3 s, send any key and expect the interrupt's `\r\n`. A first attempt timed out in the harness for an unknown reason, so it confirmed nothing. If the hypothesis holds, `signal.set_wakeup_fd` will not help, because `readline` does not `select()` on that fd. A candidate fix sets `PyOS_InputHook` with `ctypes`, which is stdlib. With a hook set, `select()` wakes every 0.1 s. Check first that it does not break the libedit build on macOS.

## Low
