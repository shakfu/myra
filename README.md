# ant

Minimal code agent in C with four tools:

| Tool    | Does                                              | Asks first |
|---------|---------------------------------------------------|------------|
| `read`  | return a file's contents                          | no         |
| `write` | create or overwrite a file                        | yes        |
| `edit`  | replace a unique `old_string` with `new_string`   | yes        |
| `shell` | run `/bin/sh` and return output plus exit status; killed after `timeout` seconds (default 120, max 600) | yes        |

Tool output is capped per provider (see the table below). Over the cap, the first fifth and the last four fifths are kept, since errors and summaries come last. Output is cleaned to valid UTF-8: invalid bytes and NUL become U+FFFD.

## Build

Requires CMake 3.16+ and libcurl; libedit, if found, adds line editing to the REPL. macOS ships both libraries. On Debian or Ubuntu: `apt install libcurl4-openssl-dev libedit-dev`. cJSON 1.7.19 (MIT) is vendored in `vendor/cjson/`. The Makefile wraps CMake and uses Ninja when installed.

    make              # configure and build into build/; ./agent links to build/agent
    make test         # ctest: unit tests plus the end-to-end suite
    make asan         # the same, in build-asan/ with AddressSanitizer and UBSan
    make clean
    make BUILD=out TYPE=Debug CMAKE_ARGS=-DAGENT_SANITIZE=ON   # overrides

CI runs `make test` and `make asan` on Linux and macOS (`.github/workflows/ci.yml`).

Layout:

| Path                  | Contains                                                         |
|-----------------------|------------------------------------------------------------------|
| `src/agent.{h,c}`     | `agentlib`: providers, tools, remembered state, streaming, the tool loop |
| `src/main.c`          | the CLI: options, provider choice, headless mode, REPL           |
| `tests/unit.c`        | `agentlib` unit tests; each `TEST(name)` is a ctest `unit.<name>` |
| `tests/test_agent.py` | end-to-end tests against a mock server; ctest `e2e`, needs `uv`  |

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

`local` covers any OpenAI-compatible server. For llama-server, start it with `--jinja` so tool calls work. Its 16 KB cap is about a quarter of a 16K-token context; raise it with `AGENT_MAX_OUTPUT` for a larger `-c`.

## Use

    export OPENROUTER_API_KEY=...
    ./agent                              # REPL
    ./agent -p "fix the build"           # headless: one task, then exit
    ./agent -m openai/gpt-5.5 -p "..."   # any OpenRouter model id; remembered
    ./agent -P local                     # llama-server on :8080; remembered
    ./agent -y -p "..."                  # no approval prompts (see below)
    ./agent -V                           # version

Replies stream to stdout as they arrive. Tool calls, prompts, errors and a per-turn `[usage]` line (tokens in, cached, out; when the server reports them) go to stderr.

REPL commands:

| Command            | Does                                                    |
|--------------------|---------------------------------------------------------|
| `/model [id]`      | show the current model, or switch to `id` (remembered)  |
| `/models [filter]` | list the provider's models whose id contains `filter`, ignoring case; `*` marks the current one |
| `/clear`           | start a new conversation; provider and model are kept   |
| `/help`            | list these commands                                     |
| `/exit`            | quit; so does Ctrl-D                                    |

On a terminal, the REPL has line editing and history (libedit). Piped input is read line by line, without a prompt.

Approval: `write`, `edit` and `shell` ask on `/dev/tty`. With no terminal they are refused. `-y` approves `shell`, and `write` and `edit` inside the working directory, with symlinks resolved. Paths outside it still ask, or are refused.

Ctrl-C:

- During a `shell` command: kills the command and its children, skips the turn's remaining tool calls, and ends the turn.
- While waiting for the model: stops the request within about a second; the turn is rolled back.
- At the REPL prompt: drops the line.
- Headless: the same, then exit with status 130.

When a request exceeds the model's context, older tool outputs are replaced by a placeholder and the request is retried once. If nothing is left to drop, the agent suggests `/clear`.

## Saved state

Under `$XDG_STATE_HOME/ant/` (default `~/.local/state/ant/`):

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
| `AGENT_MAX_OUTPUT`     | tool output cap in bytes (at least 1024), for every provider |
| `AGENT_RETRY_DELAY_MS` | first retry wait; it doubles per retry (default 1000). Tests set it to 1 |
| `XDG_STATE_HOME`       | where state is saved                                     |

## Limitations

- `-y` cannot confine `shell`: a command can still write anywhere.
- Commands cannot read the terminal. A `sudo` or `ssh` password prompt waits until the timeout.
- A background job must redirect its output (`cmd >log 2>&1 &`), or the call waits for it until the timeout.
- `edit` cannot match text that `read` showed as U+FFFD in a file that is not UTF-8. Use `shell`, e.g. `sed` or `iconv`.
- Reasoning text from thinking models is kept in the history but not shown, so a reply can start after a long silence.
