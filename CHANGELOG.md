# Changelog

## Unreleased

### Added

- `agent`, a code agent with four tools: `read`, `write`, `edit` and `shell`. It runs one task headless with `-p`, or as a REPL. Usage is in the README.

- Two providers, both on OpenAI chat completions: `openrouter`, and `local` for any OpenAI-compatible server. There is no direct Anthropic or OpenAI provider. OpenRouter serves both model families through one wire format.

- Provider selection: `-P`, then the last `-P` if it can run now, then the first cloud provider with a key. Otherwise it is an error. `local` is never a fallback: a silent fallback to `localhost:8080` fails far from its cause.

- The last provider, and each provider's last model, persist under `$XDG_STATE_HOME/ant/`. They are saved only after the server accepts a request, so a mistyped `-P` or `-m` is not kept.

- REPL commands: `/model [id]`, `/models [filter]`, `/clear` and `/exit`.

- `write`, `edit` and `shell` need approval on the terminal, or `-y`. Without a terminal they are refused, and the refusal names `-y`.

- `shell` runs each command in its own process group, with a 120 s timeout (600 s maximum). A timeout or Ctrl-C kills the whole group. As a result, commands cannot read the terminal: a `sudo` password prompt waits until the timeout.

- Ctrl-C stops the running command or request and ends the turn. A headless run then exits with status 130.

- Tool output is capped at 100 KB and cleaned to valid UTF-8. One invalid byte would otherwise make the request invalid JSON.

- Requests are retried on 429, 5xx and network errors, with back-off. A refused connection fails at once: waiting does not start a stopped server.

- OpenRouter requests carry a top-level `cache_control`, which enables prompt caching for Claude models.

- Build: CMake behind a Makefile. The core is a static library, `agentlib`, with unit and end-to-end tests under ctest. cJSON is vendored, so the binary links only system libraries.
