# myra

myra, which means *ant* in Swedish, is a minimal code agent in C with four tools:

| Tool    | Does                                              |
|---------|---------------------------------------------------|
| `read`  | return a file's contents                          |
| `write` | create or overwrite a file                        |
| `edit`  | replace a unique `old_string` with `new_string`   |
| `shell` | run `/bin/sh` and return output plus exit status; killed after `timeout` seconds (default 120, max 600) |

Tool output is capped per provider (see the table below). Over the cap, the first fifth and the last four fifths are kept, since errors and summaries come last. Output is cleaned to valid UTF-8: invalid bytes and NUL become U+FFFD.

Only what is kept is held in memory, so a huge file or a flood of output costs nothing extra. `read` skips the middle of a large file; `shell` is killed after 64 MB of output; `edit` refuses a file above 64 MB, since it rewrites the whole file. A turn stops after 100 tool calls; calls beyond that in the same reply are skipped.

## Why C

myra builds to one binary of about 119 KB (Linux, Release). It links only libc and libcurl, so no interpreter or package manager is needed.

## Build

Requires CMake 3.16+, a C11 compiler and libcurl. Everything else is optional. cJSON 1.7.19 (MIT) is vendored in `vendor/cjson/`, linenoise 2.0 (BSD-2) in `vendor/linenoise/`. The Makefile wraps CMake and uses Ninja when installed.

| Dependency   | Needed for            | Debian or Ubuntu       | macOS                    |
|--------------|-----------------------|------------------------|--------------------------|
| CMake 3.16+  | building              | `cmake`                | `brew install cmake`     |
| C11 compiler | building              | `build-essential`      | `xcode-select --install` |
| libcurl      | building              | `libcurl4-openssl-dev` | Command Line Tools       |
| Ninja        | faster builds         | `ninja-build`          | `brew install ninja`     |
| uv           | the `e2e` test        | [docs.astral.sh/uv](https://docs.astral.sh/uv/getting-started/installation/) | same |

On Debian and Ubuntu, `apt install libcurl4-openssl-dev` covers the build. Two libcurl dev packages exist, `libcurl4-openssl-dev` and `libcurl4-gnutls-dev`; they conflict, and CI uses the OpenSSL one. myra never names a TLS backend, so either compiles. Fedora calls the package `libcurl-devel`, Arch `curl`.

macOS needs only CMake from Homebrew. libcurl ships with the Command Line Tools.

Missing libcurl stops configure with `Could NOT find CURL`. A missing optional dependency prints a message and the build continues.

    make              # configure and build into build/; ./myra links to build/myra
    make test         # ctest: unit tests plus the end-to-end suite
    make asan         # the same, in build-asan/ with AddressSanitizer and UBSan
    make install      # PREFIX=/usr/local by default
    make clean
    make BUILD=out TYPE=Debug CMAKE_ARGS=-DMYRA_SANITIZE=ON   # overrides

CI runs `make test` and `make asan` on Linux and macOS (`.github/workflows/ci.yml`).

Layout:

| Path                  | Contains                                                         |
|-----------------------|------------------------------------------------------------------|
| `src/myra.{h,c}`      | `myralib`: providers, tools, remembered state, streaming, the tool loop |
| `src/main.c`          | the CLI: options, provider choice, headless mode, REPL, completion |
| `tests/unit.c`        | `myralib` unit tests; each `TEST(name)` is a ctest `unit.<name>` |
| `tests/test_agent.py` | end-to-end tests against a mock server; ctest `e2e`, needs `uv`  |
| `scripts/agent.py`    | myra in Python 3.11+, stdlib only; same options and state. ctest `e2e-py` runs the suite against it |

CMake writes `build/compile_commands.json`, which clangd finds automatically.

## Providers

Both use OpenAI chat completions, streamed.

Provider selection:

- `-P local`: local.

- `-P openrouter`: OpenRouter; exits with an error if `OPENROUTER_API_KEY` is unset.

- No `-P`: the last `-P` used, if it can run now (a cloud provider needs its key set); else the first cloud provider in the table whose key is set; else exit with an error.

`local` is only used after `-P local`, on that run or a remembered one.

| `-P`         | Key env                    | Base URL env          | Default base URL               | Default model             | Tool output cap |
|--------------|----------------------------|-----------------------|--------------------------------|---------------------------|-----------------|
| `openrouter` | `OPENROUTER_API_KEY`       | `OPENROUTER_BASE_URL` | `https://openrouter.ai/api/v1` | `anthropic/claude-opus-5` | 100 KB          |
| `local`      | `LOCAL_API_KEY` (optional) | `LOCAL_BASE_URL`      | `http://localhost:8080/v1`     | `local`                   | 16 KB           |

Model precedence: `-m`, then the last `-m` used with that provider, then the table default.

Requests to `openrouter` carry a top-level `"cache_control": {"type": "ephemeral"}`. It turns on prompt caching for Claude models, which is opt-in; other models ignore it. See [OpenRouter prompt caching](https://openrouter.ai/docs/features/prompt-caching).

`local` covers any OpenAI-compatible server. For llama-server, start it with `--jinja` so tool calls work. Its 16 KB cap is about a quarter of a 16K-token context; raise it with `MYRA_MAX_OUTPUT` for a larger `-c`.

## Use

    export OPENROUTER_API_KEY=...
    ./myra                              # REPL
    ./myra -p "fix the build"           # headless: one task, then exit
    ./myra -m openai/gpt-5.5 -p "..."   # any OpenRouter model id; remembered
    ./myra -P local                     # llama-server on :8080; remembered
    ./myra --permissions ask            # approve each write, edit and shell
    ./myra --verbose                    # full tool arguments and results
    ./myra --no-color                   # plain stderr
    ./myra -V                           # version
    ./myra --help                       # options and REPL commands

Replies stream to stdout as they arrive. Everything else goes to stderr:

- The REPL starts with `myra <version>`, then the provider and model.

- Each tool call gets one line, cut to the terminal width. `--verbose` shows the full arguments and the result instead.

- Each turn ends with `[usage]`: requests, tokens in (cached) and out, and the cost where the server reports it. OpenRouter does, in credits, which are dollars; llama-server does not. Leaving the REPL prints the `[session]` totals.

stderr is colored on a terminal. `--no-color`, or a non-empty `NO_COLOR` ([no-color.org](https://no-color.org)), turns it off.

REPL commands:

| Command            | Does                                                    |
|--------------------|---------------------------------------------------------|
| `/model [id]`      | show the current model, or switch to `id` (remembered)  |
| `/models [filter]` | list the provider's models whose id contains `filter`, ignoring case; `*` marks the current one |
| `/clear`           | start a new conversation; provider and model are kept   |
| `/help`            | list these commands                                     |
| `/exit`            | quit; so does Ctrl-D                                    |

On a terminal, the REPL has line editing, history and UTF-8 input (linenoise, vendored). Tab completes a
REPL command at the start of the line, a model id after `/model `, and otherwise a path in the working
directory; repeated tabs cycle the matches. Completing a model id costs one `/models` request, made once
per session and only if you ask for it. Piped input is read line by line, without a prompt.

`--permissions` sets when `write`, `edit` and `shell` run without asking:

| Mode             | Runs without asking                                         |
|------------------|-------------------------------------------------------------|
| `auto` (default) | all three, except `write`/`edit` outside the working directory (symlinks resolved) |
| `ask`            | none: each call asks on `/dev/tty`                          |
| `all`            | all three, anywhere                                         |
| `read-only`      | none: they are refused; `read` still runs                   |

A call that would ask is refused when there is no terminal.

The working-directory check guards against mistakes, not an adversary. `shell` can write anywhere, and a symlink swapped between the check and the write can redirect it.

`write` and `edit` replace a file atomically: a failed write leaves the old contents. The new file keeps the mode and follows symlinks, but a hard-linked file gets its own copy. A symlink whose target does not exist is refused, not followed. `edit` refuses a file containing NUL bytes, as `read` does.

Ctrl-C:

- During a `shell` command: kills the command and its children, skips the turn's remaining tool calls, and ends the turn.

- While waiting for the model: stops the request within about a second; the turn is rolled back.

- At the REPL prompt: drops the line.

- Headless: the same, then exit with status 130.

When a request exceeds the model's context, older tool outputs are replaced by a placeholder and the request is retried once. If nothing is left to drop, the agent suggests `/clear`.

## Saved state

Under `$XDG_STATE_HOME/myra/` (default `~/.local/state/myra/`):

| File                | Holds                                   |
|---------------------|-----------------------------------------|
| `provider`          | the last `-P`                           |
| `<provider>.model`  | the last model used with that provider  |
| `history`           | REPL input history, mode 0600; last 1000 lines |

`provider` and the model files are saved only after the server accepts a request, so a mistyped `-P` or `-m` is not kept.

## Environment

Besides the provider variables above:

| Variable               | Effect                                                   |
|------------------------|----------------------------------------------------------|
| `MYRA_MAX_OUTPUT`     | tool output cap in bytes (at least 1024), for every provider |
| `MYRA_RETRY_DELAY_MS` | first retry wait; it doubles per retry (default 1000). Tests set it to 1 |
| `XDG_STATE_HOME`       | where state is saved                                     |

## Limitations

- `auto` cannot confine `shell`: a command can still write anywhere. Use `ask` or `read-only` where that matters.

- `read` is not confined either: the model can read any file you can, including keys and history outside the project.

- The REPL history file holds your prompts verbatim. Delete it, or point `XDG_STATE_HOME` elsewhere, if they are sensitive.

- The atomic replacement falls back to writing in place where no temporary file can be created, such as a read-only directory holding a writable file. That path can still truncate on failure.

- Commands cannot read the terminal. A `sudo` or `ssh` password prompt waits until the timeout.

- A background job must redirect its output (`cmd >log 2>&1 &`), or the call waits for it until the timeout.

- `edit` cannot match text that `read` showed as U+FFFD in a file that is not UTF-8. Use `shell`, e.g. `sed` or `iconv`.

- Reasoning text from thinking models is kept in the history but not shown, so a reply can start after a long silence.
