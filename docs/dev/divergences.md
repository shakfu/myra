# C vs Python: behavioural differences

`scripts/agent.py` follows `src/myra.c` and `src/main.c`, and the e2e suite (`tests/test_agent.py`) runs against both. The suite does not exercise the differences below. A change that adds a difference, or removes one, updates this file.

Checked against commit `b0a3adb`.

## Intended

| Area | C (`myra`) | Python (`agent.py`) | Why |
|-|-|-|-|
| Line editing | linenoise, when stdin is a terminal | `readline` (libedit on macOS), when stdin and stdout are both terminals | `input()` edits only when stdout is a terminal |
| Prompt | stderr | stdout | `input()` writes it there |
| Lone completion | no trailing space | trailing space | `readline` default |
| Request timeout | 600 s for the whole transfer (`CURLOPT_TIMEOUT`) | 600 s per socket read (`urlopen(timeout=)`) | `urllib` has no whole-request timeout |

## Undecided

These exist in the code. Nobody has chosen them.

| Area | C | Python |
|-|-|-|
| A `tool_calls` entry that is not an object | kept in history and answered with `invalid JSON arguments for ?` | removed from the message; gets no result |
| NUL (`\u0000`) in a string argument | cJSON keeps it, so the C string ends there: `{"path":"a\u0000b"}` acts on `a` | `bad arguments for <tool>: embedded null byte` |
| Lone UTF-16 surrogate in arguments | cJSON rejects the JSON: `invalid JSON arguments for <tool>` | parses; encoding fails: `bad arguments for <tool>: ...` |
| `read` of a pipe or device longer than the cap | NUL anywhere in the bytes read makes it `is binary` | only NUL in the kept head and tail does |
| Network error line | `api error (<curl error>, http 0)[, retrying]: <body>` | `api error (<reason>)[, retrying]` |
| `shell` cannot start | `pipe failed: ...` or `fork failed: ...` | `cannot run /bin/sh: ...` |
| One-line `[tool]` log | cut to the terminal width in bytes, on a character boundary | cut in code points |
| Path completion order | `readdir` order | sorted |
