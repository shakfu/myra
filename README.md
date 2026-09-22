# ant

Minimal code agent in C with four tools:

| Tool    | Does                                              | Asks first |
|---------|---------------------------------------------------|------------|
| `read`  | return a file's contents                          | no         |
| `write` | create or overwrite a file                        | yes        |
| `edit`  | replace a unique `old_string` with `new_string`   | yes        |
| `shell` | run `/bin/sh` and return output plus exit status  | yes        |

## Build

Requires CMake 3.16+ and libcurl, which macOS ships. cJSON 1.7.19 (MIT) is vendored in `vendor/cjson/`.
The Makefile wraps CMake and uses Ninja when installed.

    make              # configure and build into build/; the binary is build/agent
    make test         # ctest: unit tests plus the end-to-end suite
    make asan         # the same, in build-asan/ with AddressSanitizer and UBSan
    make clean
    make BUILD=out TYPE=Debug CMAKE_ARGS=-DAGENT_SANITIZE=ON   # overrides

Layout:

| Path                  | Contains                                                         |
|-----------------------|------------------------------------------------------------------|
| `src/agent.{h,c}`     | `agentlib`: providers, tools, remembered state, the tool loop    |
| `src/main.c`          | the CLI: options, provider choice, headless mode, REPL           |
| `tests/unit.c`        | `agentlib` unit tests; each `TEST(name)` is a ctest `unit.<name>` |
| `tests/test_agent.py` | end-to-end tests against a mock server; ctest `e2e`, needs `uv`  |

CMake writes `build/compile_commands.json`, which clangd finds automatically.

## Providers

Both use OpenAI chat completions.

Provider selection:

- `-P local`: local.
- `-P openrouter`: OpenRouter; exits with an error if `OPENROUTER_API_KEY` is unset.
- No `-P`: the first cloud provider in the table whose key is set; else exit with an error.

`local` is never chosen implicitly.

| `-P`                   | Key env                     | Base URL env          | Default base URL               | Default model             |
|------------------------|-----------------------------|-----------------------|--------------------------------|---------------------------|
| `openrouter`           | `OPENROUTER_API_KEY`        | `OPENROUTER_BASE_URL` | `https://openrouter.ai/api/v1` | `anthropic/claude-opus-5` |
| `local`                | `LOCAL_API_KEY` (optional)  | `LOCAL_BASE_URL`      | `http://localhost:8080/v1`     | `local`                   |

Model precedence: `-m`, then the last `-m` used with that provider, then the table default.
The model is saved to `$XDG_STATE_HOME/ant/<provider>.model` (default `~/.local/state/ant/`).
It is saved only after the server accepts a request, so a mistyped `-m` is not kept.

Requests to `openrouter` carry a top-level `"cache_control": {"type": "ephemeral"}`.
It turns on prompt caching for Claude models, which is opt-in; other models ignore it.
See [OpenRouter prompt caching](https://openrouter.ai/docs/features/prompt-caching).

`local` covers any OpenAI-compatible server. For llama-server, start it with `--jinja` so tool calls work.

## Use

    export OPENROUTER_API_KEY=...
    build/agent                              # REPL; /exit or EOF quits
    build/agent -p "fix the build"           # headless: one task, then exit
    build/agent -m openai/gpt-5.5 -p "..."   # any OpenRouter model id; remembered
    build/agent -P local                     # llama-server on :8080
    build/agent -y -p "..."                  # run write/edit/shell without asking

REPL commands:

| Command            | Does                                                    |
|--------------------|---------------------------------------------------------|
| `/model [id]`      | show the current model, or switch to `id` (remembered)  |
| `/models [filter]` | list the provider's models whose id contains `filter`; `*` marks the current one |
| `/exit`            | quit                                                    |

Model text goes to stdout. Tool calls, prompts and errors go to stderr.
Confirmation reads from `/dev/tty`. With no terminal and no `-y`, mutating tools are denied.
