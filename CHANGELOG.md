# Changelog

## 0.1.0 - 2026-09-22

### Added

- `agent`, a code agent with four tools: `read`, `write`, `edit` and `shell`. It runs one task headless with `-p`, or as a REPL. Usage is in the README.

- Two providers, both on OpenAI chat completions: `openrouter`, and `local` for any OpenAI-compatible server. There is no direct Anthropic or OpenAI provider. OpenRouter serves both model families through one wire format.

- Provider selection: `-P`, then the last `-P` if it can run now, then the first cloud provider with a key. Otherwise it is an error. `local` is never a fallback: a silent fallback to `localhost:8080` fails far from its cause.

- The last provider, and each provider's last model, persist under `$XDG_STATE_HOME/ant/`. They are saved only after the server accepts a request, so a mistyped `-P` or `-m` is not kept.

- Replies stream as they arrive. Reasoning text is kept for the history but not shown. A failed stream is not retried once text has been printed, so nothing prints twice.

- REPL commands: `/model [id]`, `/models [filter]`, `/clear`, `/help` and `/exit`. On a terminal the REPL has line editing and history via libedit, which is optional at build time.

- `write`, `edit` and `shell` need approval on the terminal, or `-y`. Without a terminal they are refused, and the refusal names `-y`. `-y` covers `write` and `edit` only inside the working directory; `shell` cannot be confined, so it stays covered.

- `shell` runs each command in its own process group, with a 120 s timeout (600 s maximum). A timeout or Ctrl-C kills the whole group. As a result, commands cannot read the terminal: a `sudo` password prompt waits until the timeout.

- Ctrl-C stops the running command or request and ends the turn. A headless run then exits with status 130.

- Tool output is cleaned to valid UTF-8, since one invalid byte made the request invalid JSON. It is capped per provider: 100 KB for `openrouter`, 16 KB for `local`, whose usual context is 16K tokens. Over the cap, the first fifth and the last four fifths are kept, because errors come last.

- When a request exceeds the model's context, older tool outputs are replaced by a placeholder and the request is retried once. If nothing is left to drop, the agent suggests `/clear`.

- Each turn ends with a `[usage]` line on stderr: tokens in, cached and out, summed over the turn's requests.

- Requests are retried on 429, 5xx and network errors, with back-off. A refused connection fails at once: waiting does not start a stopped server.

- OpenRouter requests carry a top-level `cache_control`, which enables prompt caching for Claude models.

- Build: CMake behind a Makefile. The core is a static library, `agentlib`, with unit and end-to-end tests under ctest; CI runs them on Linux and macOS, also under AddressSanitizer. cJSON is vendored, so the binary links only system libraries.
