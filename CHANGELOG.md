# Changelog

## Unreleased

### Changed

- Line editing comes from linenoise, vendored in `vendor/linenoise/` (BSD-2), instead of system libedit. libedit was optional, so a build that could not find it had no line editing at all, and it forced a `siglongjmp` out of the SIGINT handler: libedit retries `read()` after `EINTR`, which swallowed the first Ctrl-C. linenoise hands over the read loop, so Ctrl-C at the prompt is an ordinary keystroke and the jump is gone. The binary grows about 21 KB net and stops linking libedit, libtinfo, libbsd and libmd.

- Builds compile with `-ffunction-sections -fdata-sections` and link with `--gc-sections`, or `-dead_strip` on macOS. That drops 20 KB the program never reaches, most of it linenoise's blocking API, mouse and multiline modes and its allocator hook. Deleting those from `vendor/linenoise/` would have saved the same bytes while making the copy diverge from upstream.

### Added

- Tab completion in the REPL: a command at the start of a line, a model id after `/model `, a path anywhere else. Model ids cost one `/models` request, made once per session and only when a completion asks for them.

- `myra_model_ids` and `myra_free_model_ids` return the provider's model ids; `myra_list_models` now prints that list rather than fetching its own.

### Fixed

- A failed `cmake` configure left `build/CMakeCache.txt` behind, so the next `make` treated the tree as configured and reported a missing `Makefile` instead of the real error. `.DELETE_ON_ERROR` drops the half-written cache.

## 0.1.2

### Changed

- Renamed to `myra`, Swedish for ant. The binary, the state directory (`~/.local/state/myra/`) and the environment variables (`MYRA_MAX_OUTPUT`, `MYRA_RETRY_DELAY_MS`) follow the name. Move the old state directory across to keep a remembered provider and model. The C API follows: `myra.h`, `myralib`, the `myra_agent` type, and the `myra_`/`MYRA_` prefixes, including CMake options `MYRA_SANITIZE` and `MYRA_WERROR`. The old `agent` names were generic enough to clash when embedded.

- String literals passed to `buf_add` and `strncmp` take their length from `sizeof`, via `buf_lit` and `MYRA_STARTS_WITH`. The rename left three hand-written lengths stale. One of them cut the state path at `myra/`, so no state was saved.

### Fixed

- GCC builds with `-DMYRA_WERROR=ON` failed on Linux. GCC could not rule out `b->n + n + 1` wrapping to 0 in `buf_add`, and warned of a `memcpy` of `SIZE_MAX` bytes. `buf_add` now exits on overflow instead. A unit test also ignored the result of `truncate`, which glibc marks `warn_unused_result`; that call was dead and is removed.

## 0.1.1

### Added

- The REPL starts with `ant agent <version>`; `-V` prints the same.

- One stderr line per tool call, cut to the terminal width, since long `shell` commands buried the conversation. `--verbose` shows the full arguments and each result.

- The cost of each turn and, on leaving the REPL, the session's totals, where the server reports cost. OpenRouter does; llama-server does not.

- `make install` (`PREFIX=/usr/local`), and `-Werror` for our own targets in CI via `-DAGENT_WERROR=ON`.

- Color on stderr when it is a terminal. `--no-color` or `NO_COLOR` turns it off. The prompt stays plain: libedit miscounts its width when it holds escape codes.

### Changed

- Tools no longer ask for approval by default. `--permissions` selects the mode: `auto` (the default), `ask`, `all` or `read-only`. `auto` is what `-y` did: no prompts, except `write` and `edit` outside the working directory. Per-call prompts on a 5-20 call task made `-y` the only practical choice, so it became the default.

### Fixed

- `read` and `shell` held all their output in memory before the cap was applied, so one large file or a fast-producing command could exhaust it. Only the kept head and tail are stored now; `read` seeks past the middle of a large file, `shell` is killed after 64 MB, and `edit`, which must hold the whole file, refuses one above 64 MB.

- Nothing bounded the tool loop: a model that kept asking for tools ran until interrupted. A turn now stops after 100 calls.

- `AGENT_RETRY_DELAY_MS` was parsed with `atol` and shifted per retry, which overflowed on a huge value. It is parsed with `strtol` and clamped to a minute.

- A final stream event without its blank line was dropped, and a stream cut off mid-reply was treated as complete. The remainder is now parsed, and a stream with no `finish_reason` and no `[DONE]` counts as failed: retried when nothing had been printed.

- A failed `write` or `edit`, e.g. on a full disk, could leave the file empty or truncated: it was opened for writing before the new contents existed. Files are now replaced atomically, via a temporary file and a rename. A hard-linked file therefore gets its own copy.

- A tool call without an `id`, which some OpenAI-compatible servers send, produced a result the next request could not match, so the server rejected it and the turn was lost. Such calls now get a generated id.

- A `shell` `timeout` outside the range of `int` was undefined behaviour; it is now clamped first.

### Removed

- `-y`. What it did is now the default, `--permissions auto`; scripts that pass `-y` must drop it.

## 0.1.0

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
