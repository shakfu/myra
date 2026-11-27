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

myra builds to one binary of about 135 KB (Linux, Release). It links only libc and libcurl, so no interpreter or package manager is needed.

## Build

Requires CMake 3.16+, a C11 compiler and libcurl. Everything else is optional. cJSON 1.7.19 (MIT) is vendored in `vendor/cjson/`, linenoise 2.0 (BSD-2) in `vendor/linenoise/`, and CUI 1.3.1 (MIT, used only by `ui/`) in `vendor/cui/`. The Makefile wraps CMake and uses Ninja when installed.

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
    make test-ui      # the same plus myra-ui and its tests, in build-ui/ (see "Desktop UI")
    make ui           # build and run myra-ui; ARGS='-P local' passes options
    make install      # stripped, to PREFIX/bin; PREFIX=~/.local by default
    make install-ui   # the same for myra and myra-ui, built in build-ui/
    make install-lib  # libmyra.a, include/myra/ and a CMake package, to PREFIX
    make vendor-cui VERSION=1.x.y   # replace vendor/cui/ with that CUI release
    make clean
    make BUILD=out TYPE=Debug CMAKE_ARGS=-DMYRA_SANITIZE=ON   # overrides

CI runs `make test` and `make asan` on Linux and macOS (`.github/workflows/ci.yml`).

Layout:

| Path                  | Contains                                                         |
|-----------------------|------------------------------------------------------------------|
| `include/myra/`       | public headers: `myra.h` (agent, tools, loop), `mcp.h` (MCP client), `term.h` (terminal front end) |
| `lib/`                | `myralib` (`libmyra.a`). Prints nothing. `agent.c`: lifecycle, tool registry, permission; `loop.c`: a turn; `api.c`: providers, HTTP and streaming; `tools.c`: the coding tools; `state.c`: remembered state and sessions; `text.c`: shared helpers; `mcp.c`: MCP client |
| `term/`               | `libmyra-term`: escaping, markdown, notes on stderr, replies on stdout, the `/dev/tty` prompt, as `myralib` hooks |
| `cli/main.c`          | `myra`, the agent executable: options, provider choice, headless mode, REPL, completion |
| `cli/acp.{h,c}`       | `--acp`: the Agent Client Protocol server                        |
| `cli/family.{h,c}`    | `-r`: recursive agents                                           |
| `swarm/`              | `myra-swarm`: an agent per part of a markdown plan, in parallel, as child processes or threads; ctest `swarm` |
| `tests/consumer/`     | a program built against an installed `libmyra`; ctest `install.consumer` |
| `tests/unit.c`        | `myralib` unit tests; each `TEST(name)` is a ctest `unit.<name>` |
| `tests/test_agent.py` | end-to-end tests against a mock server; ctest `e2e`, needs `uv`  |
| `scripts/agent.py`    | myra in Python 3.11+, stdlib only; same options and state. ctest `e2e-py` runs the suite against it |
| `ui/`                 | `libmyra-ui`, the CUI window (`app.h`, `ui_run`), and `myra-ui`, its executable (`main.c`); ctest `ui.*` |

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
    ./myra "fix the build"              # REPL, with this as the first line
    ./myra -- "-v crashes; fix it"      # -- before a prompt that starts with -
    ./myra -C ~/src/app -p "..."        # run in another directory
    ./myra --json -p "..."              # headless, with a JSON result line on stdout
    ./myra -m openai/gpt-5.5 -p "..."   # any OpenRouter model id; remembered
    ./myra -P local                     # llama-server on :8080; remembered
    ./myra --permissions ask            # approve each write, edit and shell
    ./myra --mcp-config mcp.json        # add MCP servers' tools
    ./myra -r --budget-tokens 500000    # let the agent start child agents
    ./myra --verbose                    # full tool arguments and results
    ./myra --no-color                   # plain stderr
    ./myra --raw                        # raw markdown on stdout
    ./myra -V                           # version
    ./myra --help                       # options and REPL commands

Replies stream to stdout as they arrive. On a terminal, their markdown is rendered with ANSI styles: headings, bold, italic, inline and fenced code, bullets, quotes and rules. Fence lines are hidden. Everything else goes to stderr:

- The REPL starts with `myra <version>`, then the provider and model.

- Each tool call gets one line, cut to the terminal width. `--verbose` shows the full arguments and the result instead.

- Each turn ends with `[usage]`: requests, tokens in (cached) and out, and the cost where the server reports it. OpenRouter does, in credits, which are dollars; llama-server does not. Leaving the REPL prints the `[session]` totals.

stderr is colored when it is a terminal, and markdown is rendered when stdout is. `--no-color` turns off color, `--raw` turns off rendering, and a non-empty `NO_COLOR` ([no-color.org](https://no-color.org)) turns off both.

Control characters from the model or a tool are shown in caret notation on a terminal: ESC as `^[`, DEL as `^?`, C1 as `M-^[`. So a reply, `[tool]` line or `--verbose` result cannot move the cursor, set the title or write the clipboard. A lone `\r` shows as `^M`; `\r\n` stays a line break. Piped output is not escaped. No option turns this off; pipe the output to get the raw bytes.

REPL commands:

| Command            | Does                                                    |
|--------------------|---------------------------------------------------------|
| `/model [id]`      | show the current model, or switch to `id` (remembered)  |
| `/models [filter]` | list the provider's models whose id contains `filter`, ignoring case; `*` marks the current one |
| `/clear`           | start a new conversation; provider and model are kept   |
| `/raw`             | print the last reply as raw markdown, for copying       |
| `/help`            | list these commands                                     |
| `/exit`            | quit; so does Ctrl-D                                    |

On a terminal, the REPL has line editing, history and UTF-8 input (linenoise, vendored). Tab completes a REPL command at the start of the line, a model id after `/model `, and otherwise a path in the working directory; repeated tabs cycle the matches. Completing a model id costs one `/models` request, made once per session and only if you ask for it. Piped input is read line by line, without a prompt.

Words after the options are joined by single spaces and taken as the REPL's first line, as if typed: a line of blanks is skipped, and `/exit` or another command runs as one. The REPL then reads stdin as usual. They cannot be combined with `-p` or `--acp`.

`-C DIR` (`--cwd DIR`) changes to DIR before anything else, so tools, the system prompt and a relative `--mcp-config` path all see DIR. A missing DIR is an error, exit status 2. Under `--acp` it is rejected: each session's cwd comes from the editor.

`--json` needs `-p`. stdout then holds one line, the result:

    {"type": "result", "outcome": "complete", "text": "...", "error": null, "cost": 0.0123,
     "requests": 2, "input_tokens": 2100, "output_tokens": 25, "cached_tokens": 900,
     "model": "...", "provider": "openrouter", "session_id": "..."}

- `outcome` is `complete`, `truncated` (a reply hit the length limit or the turn hit the 100 tool-call cap), `error` or `cancelled` (Ctrl-C).
- `text` is this run's last assistant message with text, or `""`. `error` is the error message, or null unless `outcome` is `error`.
- `cost` is null when the server reports none. `requests` and the token counts are those of the `[usage]` line, which counts only responses that report usage.
- A setup failure after the options are accepted, such as a bad `-C` or a missing key, also prints a result line. Invalid options print none.
- `session_id` names the saved session that holds the conversation, or is null if none does.
- Exit status is as for `-p`. stderr is unchanged.

### Sessions: `--resume`

With `--json`, `-p` saves the conversation as a new session, named by `session_id`. `--resume ID -p TEXT` loads session ID, sends TEXT as the next turn, and saves the session again under the same ID:

    id=$(./myra --json -p "read main.c" | jq -r .session_id)
    ./myra --resume "$id" --json -p "now add a test"

- A session is `$XDG_STATE_HOME/myra/sessions/ID.json`, mode 0600. It holds every message but the system prompt: tool calls, tool results and reasoning payloads, so the next turn replays them.
- A session holds whatever the tools printed, secrets included. `--no-save` saves nothing.
- A resumed run keeps the session's provider and model, unless `-P` names another provider or `-m` another model. The model is not remembered as the provider's default.
- A failed turn is rolled back and not saved; `session_id` still names the session as it was. A cancelled turn is saved.
- An unknown ID, or a session saved in another working directory, is an error, exit status 1, since the history names files there.
- `--resume` needs `-p`. Plain `-p`, without `--json` or `--resume`, saves nothing: nothing reports the id.
- Two runs that resume one session at once both save it; the last to finish wins.
- Sessions are never deleted. Remove old files by hand.

`--permissions` sets when `write`, `edit` and `shell` run without asking:

| Mode             | Runs without asking                                         |
|------------------|-------------------------------------------------------------|
| `auto` (default) | all three, except `write`/`edit` outside the working directory (symlinks resolved) |
| `ask`            | none: each call asks on `/dev/tty`                          |
| `all`            | all three, anywhere                                         |
| `read-only`      | none: they are refused; `read` still runs                   |

A call that would ask is refused when there is no terminal.

The permission prompt always escapes control characters, `\n` included (`^J`), so a `\r` or erase-line in a command cannot hide what runs.

The working-directory check guards against mistakes, not an adversary. `shell` can write anywhere, and a symlink swapped between the check and the write can redirect it.

`write` and `edit` replace a file atomically: a failed write leaves the old contents. The new file keeps the mode and follows symlinks, but a hard-linked file gets its own copy. A symlink whose target does not exist is refused, not followed. `edit` refuses a file containing NUL bytes, as `read` does.

Ctrl-C:

- During a `shell` command: kills the command and its children, skips the turn's remaining tool calls, and ends the turn.

- While waiting for the model: stops the request within about a second; the turn is rolled back.

- At the REPL prompt: drops the line.

- Headless: the same, then exit with status 130.

SIGTERM and SIGHUP stop the turn as Ctrl-C does. Then the REPL, `-p` or `--acp` ends, and myra exits with status 143 or 129.

When myra exits, by any route but SIGKILL, it kills what its `shell` calls left running in the background. Each call's process group gets SIGTERM, then SIGKILL 1 s later. A job that leaves its process group, for example through `setsid`, survives.

When a request exceeds the model's context, older tool outputs are replaced by a placeholder and the request is retried once. If nothing is left to drop, the agent suggests `/clear`. If the turn fails anyway, the outputs are restored.

### MCP servers: `--mcp-config`

`--mcp-config FILE` starts the [MCP](https://modelcontextprotocol.io) servers listed in FILE, in the format other MCP clients use:

    {"mcpServers": {"git": {"command": "uvx", "args": ["mcp-server-git"], "env": {"K": "v"}}}}

Their tools join the four built-ins as `mcp__<server>__<tool>`. Only stdio servers are supported. A server inherits only `HOME`, `LOGNAME`, `PATH`, `SHELL`, `TERM` and `USER` from myra's environment, plus its own `env`, as the MCP SDKs do; API keys stay out. A server that fails to start is reported on stderr and left out; Ctrl-C while servers start exits with status 130. `--permissions` treats MCP tools like `shell`: `read-only` denies them and `ask` asks for each call. A call is stopped by Ctrl-C and after 600 s. The servers stop when myra exits.

A project `.mcp.json` is not read unless named: a cloned repository could otherwise make myra run its commands.

### Editors: `--acp`

`myra --acp` serves the [Agent Client Protocol](https://agentclientprotocol.com) on stdin and stdout, so an editor can run it, for example loki's `:agent`. Provider, model and `--permissions` work as in the REPL.

If the editor serves files (`fs.readTextFile` and `fs.writeTextFile`):
- `read` goes through the editor, so it sees unsaved buffers. If the editor refuses a path inside the working directory, for example a binary file, `read` falls back to the disk. Outside it, the refusal and its reason go to the model, except under `--permissions all`, which reads the disk.
- `write` and `edit` go through the editor, and its refusal is an error.
- `shell` always runs locally.

A call that `--permissions` would ask about on the terminal is asked in the editor instead, as `session/request_permission`. So `--permissions ask` works there.

`session/cancel` acts like Ctrl-C: it kills a running `shell` command or stops the model request, and the turn ends with `stopReason` `cancelled`. Reply text and each tool call stream as `session/update` messages. A tool call is reported `completed` or `failed` with its result; one waiting for approval is `pending` until allowed. stdout carries only protocol messages; notes go to stderr as usual.

There is one session at a time. `session/new` changes to the session's `cwd` and starts a new conversation.

A `usage_update` ends each turn that the server reported usage for: `used` is the last response's prompt and completion tokens, `size` the model's context window, and `cost` the session's total, in `USD` on OpenRouter and `credits` elsewhere. The window comes from the provider's `/models` entry, looked up once; OpenRouter gives one, most local servers do not. `--context-window TOKENS` gives it instead. Without a window, no `usage_update` is sent, since `size` is required.

Each session starts the `--mcp-config` servers, then those listed in `session/new`, in the session's `cwd`. They work as described under `--mcp-config`, with `session/cancel` in place of Ctrl-C. myra declares no `http` or `sse` MCP capability. A new session stops the previous servers.

### Recursive agents: `-r`

`-r` lets the agent start child agents through `shell` and collect their answers:

    myra -r --budget-tokens 500000 -p "review src/ with one child per file"

The system prompt tells the agent these commands:

| Command | Does |
|-|-|
| `myra -r spawn --name NAME [-m MODEL] -- TASK` | start a headless child on TASK; prints its id |
| `myra -r wait NAME_OR_ID [--timeout SECS]` | print the child's answer, waiting up to 100 s by default |
| `myra -r agents` | list the family |
| `myra -r shutdown [--grace SECS]` | stop the caller's children and theirs |

- A child gets its parent's provider, model and `--permissions`, and runs in the directory `spawn` ran in. Under `ask`, `spawn` is refused, since a child cannot ask anyone.
- When a child finishes, a note follows the parent's next tool result. The answer comes only from `wait`, as tool output.
- A child stops within about a second of its parent's exit.
- `--budget-tokens N` is required: every request in the family reserves its input and output first, and goes out with that output as `max_tokens`. A request that would cross the budget fails and stops the requesting agent's children. `--budget-cost USD` adds a limit at the rates OpenRouter lists.
- State is files under `$XDG_STATE_HOME/myra/family/`, removed when the root exits. Each child saves its session under its id, for `--resume`.

Design and limits: `docs/dev/recursion.md`.

### Desktop UI: `myra-ui`

`make ui` builds and runs `build-ui/ui/myra-ui`, `make test-ui` tests it and `make install-ui` installs it, a window on the same `myralib`, drawn by [CUI](https://github.com/yappologistic/CUI). The CLI build does not need it. Linux also needs the X11 headers (`libx11-dev`).

    myra-ui [-P provider] [-m model] [--permissions MODE] [-C DIR] [PROMPT...]

Options work as for `myra`; words after them are sent as the first message. `--permissions` defaults to `ask`, since a window keeps no scrollback to review changes in afterwards. The header switches it for the next turn.

The window has:

- the chat, with replies as markdown, reasoning in a collapsible block, and each tool call with its status, time, input and output
- an inline diff under each `write` and `edit`, also shown when asking to allow one
- a file browser of the working directory; double-click adds a path to the prompt
- a log of myra's notes (`[tool]`, `[usage]`, errors); errors also show as a toast
- a model picker over the provider's `/models`, and a folder picker that starts a new chat there
- a command palette (Cmd+K on macOS, Ctrl+K elsewhere); Esc stops a turn

Panels dock and rearrange by dragging their tabs. Closing the window during a turn interrupts it and waits for it to end, so a running `write` or `shell` finishes first.

### Parallel agents: `myra-swarm`

`myra-swarm PLAN.md` splits one objective across agents. The plan has one `#` heading, the objective, and a `##` heading per part. Text under a heading gives it context. One agent works on each part, all at once unless `-j N` limits them. When all have finished, one more agent combines their answers. stdout gets a markdown report: the combined answer, then each part's answer. `--json` prints the same as one object. Progress goes to stderr. Each part's agent sees the objective, its own part and the other parts' titles.

    # Why does the release build take 9 minutes?

    CI on main, since commit 49691ed.

    ## Compile time
    Which targets dominate? Use the build logs in build/.

    ## Tests
    Which ctest entries take longest, and do they need to run serially?

    ## Fix
    depends: Compile time, Tests
    Propose the three changes that save the most time.

    myra-swarm -P openrouter plan.md > report.md

A part's `depends: TITLE, ...` line starts it only after those parts finish, and puts their answers in its prompt. If a dependency fails, the part is skipped. Titles must match exactly. Unknown titles, cycles and duplicate titles are refused before anything runs. Each part is asked for an answer under 400 words, which bounds what the combining agent reads; `--words N` changes the limit, and `--words 0` drops it. The limit is a request, not enforced.

`--mode processes`, the default, runs each agent as a child `myra -p --json` (found in `PATH`, or named by `--myra PATH`). Agents then share nothing but the working directory. `--mode threads` runs each on a thread of `myra-swarm` itself, on `myralib`. There they share its process-wide state, with the bugs `docs/dev/agent-context.md` lists; it exists to test the library. In processes mode a prompt is one argument, so it is capped at 120 KiB, below Linux's 128 KiB per argument.

`--budget-tokens N` (and optionally `--budget-cost USD`) caps what all the agents together spend, in processes mode only. `myra-swarm` starts `myra -r host`, and each agent runs as a member of that family, reserving every request against one shared ledger. A request that would cross the budget fails, and so does its agent. Members also get the family's system prompt, so a part can start sub-agents of its own under the same budget (see "Recursive agents").

The agents share the working directory and the `--permissions` mode (default `auto`; `ask` is refused, since there is no one to ask). Exit status 1 means some part or the combination failed; the report says which. Ctrl-C stops every agent and skips the combination. It is built by `make` as `build/swarm/myra-swarm` and not installed. `--tasks` runs JSON-line tasks instead, for the tests in `swarm/tests/`, which run every case in both modes.

### Library: `libmyra`

`myra` and `myra-ui` are executables on `myralib`. `make install-lib` installs it for other programs: `libmyra.a`, `libmyra-term.a`, the headers in `PREFIX/include/myra/`, and a CMake package. The archive bundles cJSON; include it as `<myra/cJSON.h>`. The API is in `include/myra/myra.h`; it may change in any 0.x minor release.

    find_package(myra 0.2 REQUIRED)   # CMAKE_PREFIX_PATH=~/.local
    target_link_libraries(app PRIVATE myra::myra)   # or myra::term, for a terminal

    #include <myra/myra.h>

`myralib` prints nothing. A front end sees a turn through `myra_io` hooks and `myra_note_hook`; without them, reply text and notes are dropped, and `myra_last_error` keeps the last error. A permission question with no `allow` hook is denied. `libmyra-term` (`myra/term.h`) supplies terminal hooks: `myra_term_notes` prints notes on stderr, and `myra_term_attach` prints replies on stdout and asks on `/dev/tty`.

An agent starts with no tools. `myra_add_coding_tools` gives it `read`, `write`, `edit` and `shell` and the coding agent's system prompt; `myra_add_tool` adds any other. A `myra_tool` names its JSON Schema, the argument shown for a call, and an access class, `MYRA_TOOL_READS`, `_WRITES` or `_RUNS`. The access class decides when `--permissions` asks. The core checks required arguments before `run` is called. The model sees only registered tools, and a call to any other name is refused.

The bundled cJSON symbols clash with a second cJSON linked into the same program.

## Saved state

Under `$XDG_STATE_HOME/myra/` (default `~/.local/state/myra/`):

| File                | Holds                                   |
|---------------------|-----------------------------------------|
| `provider`          | the last `-P`                           |
| `<provider>.model`  | the last model used with that provider  |
| `history`           | REPL input history, mode 0600; last 1000 lines |
| `sessions/ID.json`  | saved conversations, mode 0600; see "Sessions" |
| `family/ID/`        | a running `-r` family's registry, ledger, answers and logs, mode 0700 |
| `ui.theme`, `ui.layout` | `myra-ui`'s light or dark theme and panel layout |

`provider` and the model files are saved only after the server accepts a request, so a mistyped `-P` or `-m` is not kept.

## Environment

Besides the provider variables above:

| Variable               | Effect                                                   |
|------------------------|----------------------------------------------------------|
| `MYRA_MAX_OUTPUT`     | tool output cap in bytes (at least 1024), for every provider |
| `MYRA_KEEP_FAMILY`    | non-empty: a `-r` root leaves its family's files at exit, for inspection; the next root removes them |
| `MYRA_RETRY_DELAY_MS` | first retry wait; it doubles per retry (default 1000). Tests set it to 1 |
| `XDG_STATE_HOME`       | where state is saved                                     |

## Limitations

- `auto` cannot confine `shell`: a command can still write anywhere. Use `ask` or `read-only` where that matters.

- `read` is not confined either: the model can read any file you can, including keys and history outside the project.

- The REPL history file holds your prompts verbatim, and session files hold tool output too. Delete them, or point `XDG_STATE_HOME` elsewhere, if they are sensitive.

- The atomic replacement falls back to writing in place where no temporary file can be created, such as a read-only directory holding a writable file. That path can still truncate on failure.

- Commands cannot read the terminal. A `sudo` or `ssh` password prompt waits until the timeout.

- A background job must redirect its output (`cmd >log 2>&1 &`), or the call waits for it until the timeout. It stops when myra exits.

- `edit` cannot match text that `read` showed as U+FFFD in a file that is not UTF-8. Use `shell`, e.g. `sed` or `iconv`.

- Markdown rendering is a subset. Tables, links and emphasis across lines stay raw. A `*`, `_` or backtick is decided without seeing the rest of the line, so an unclosed one styles the line to its end.

- Reasoning text from thinking models is kept in the history but not shown, so a reply can start after a long silence.
