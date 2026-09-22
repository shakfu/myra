# ant

Minimal code agent in C with four tools:

| Tool    | Does                                              | Asks first |
|---------|---------------------------------------------------|------------|
| `read`  | return a file's contents                          | no         |
| `write` | create or overwrite a file                        | yes        |
| `edit`  | replace a unique `old_string` with `new_string`   | yes        |
| `shell` | run `/bin/sh` and return output plus exit status  | yes        |

## Build

Requires libcurl and cJSON (`brew install cjson`).

    make
    make test    # pytest via uv, against a mock API server

## Providers

Select with `-P`; override the model with `-m`.

| `-P`                 | Key env              | Base URL env          | Default base URL               | Default model             |
|----------------------|----------------------|-----------------------|--------------------------------|---------------------------|
| `openrouter` (default) | `OPENROUTER_API_KEY` | `OPENROUTER_BASE_URL` | `https://openrouter.ai/api/v1` | `anthropic/claude-opus-5` |
| `anthropic`          | `ANTHROPIC_API_KEY`  | `ANTHROPIC_BASE_URL`  | `https://api.anthropic.com`    | `claude-opus-5`           |
| `openai`             | `OPENAI_API_KEY`     | `OPENAI_BASE_URL`     | `https://api.openai.com/v1`    | none; `-m` required       |
| `compat`             | `COMPAT_API_KEY` (optional) | `COMPAT_BASE_URL` | `http://localhost:8080/v1` | `local`                   |

`anthropic` uses the Messages API. The others use OpenAI chat completions.
`compat` covers any OpenAI-compatible server. For llama-server, start it with `--jinja` so tool calls work.

## Use

    export OPENROUTER_API_KEY=...
    ./agent                              # REPL; /exit or EOF quits
    ./agent -p "fix the build"           # headless: one task, then exit
    ./agent -P anthropic -p "..."
    ./agent -P openai -m gpt-5.5 -p "..."
    ./agent -P compat                    # llama-server on :8080
    ./agent -y -p "..."                  # run write/edit/shell without asking

Model text goes to stdout. Tool calls, prompts and errors go to stderr.
Confirmation reads from `/dev/tty`. With no terminal and no `-y`, mutating tools are denied.
