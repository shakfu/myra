# New features for myra

myra is deliberately small: four tools, two OpenAI-compatible providers, one streaming tool loop, a cursed-but-tiny C binary. That restraint is the point, so the features below are chosen to add value *without* turning it into a framework.

They are grouped by the axis they change, and each entry is meant to be orthogonal to the others: implementing any one does not require or duplicate the next. Within a group the items still stand alone unless noted.

Each item names the axis, the user-visible win, and roughly where it lands in the current code (`src/myra.c` = providers/tools/loop, `src/main.c` = CLI/REPL).

---

## 1. Capability: better read-side tools

The model currently discovers and reads code by shelling out to `grep`, `find` and `sed`. That is powerful but noisy: shell output is capped at 100 KB / 16 KB, is not path-aware, and cannot be approved finely. Read-side tools are the cheapest capability win because they are pure functions with no write risk.

### 1.1 `list` — directory listing
Axis: **tool surface**. A `list(path)` tool returning names, sizes and modes, one per line, sorted, recursing one level by default and refusing to descend outside the working directory. Removes the most common `ls -la` shell call and gives the model stable, parseable output. Thin wrapper over `opendir`, sharing the `read` path-safety checks.

### 1.2 `grep` — content search
Axis: **tool surface**. `grep(pattern, path?, glob?)` returning `path:line:text`, matching the file walk used by `read`. It can reuse the existing output cap and truncation logic. This is orthogonal to `list` (one walks names, the other contents) and to `read` (one locates, the other fetches).

### 1.3 `read` ranges and counts
Axis: **existing tool**. Add optional `offset`/`limit` (lines) so a large file can be read in slices instead of skipping the middle. Purely an argument addition to `read`, useful on its own even if no new tool ships.

### 1.4 `fetch` — HTTP GET as a tool
Axis: **tool surface**. A read-only `fetch(url)` returning status, content type and a capped, UTF-8 cleaned body. myra already links libcurl, so this adds no dependency. It makes the agent useful for docs and error messages without a shell `curl`, and it is a natural place to hang the permission model. Distinct from `grep`/`list` because it leaves the filesystem.

---

## 2. Context and cost management

The context-full fallback (drop old tool output, retry once) is coarse. These features give the user control over what is in context and what it costs.

### 2.1 Project instruction file
Axis: **prompting**. Read `MYRA.md` (and/or `AGENTS.md`) from the working directory, or any ancestor, and prepend it to the system prompt. One file read at startup; a well-known convention that lets a repo describe its build, test and style rules. Independent of any tool change.

### 2.2 Token/context budget indicator
Axis: **observability**. Show, per turn, how full the context is (tokens used vs. the model's window), not just the tokens in/out already reported. A `/context` REPL command could print the breakdown by message. Requires knowing the window size, which can be a per-model config default. Orthogonal to cost reporting.

### 2.3 Manual compaction
Axis: **context control**. A `/compact` command that summarizes older turns (one extra model call) and replaces them, and/or pins chosen messages so the automatic drop never removes them. Turns the silent "placeholders" recovery into something the user drives.

### 2.4 Cost/token budget ceiling
Axis: **cost control**. `--max-cost` / `--max-tokens` that stop the turn (or the session) when the accumulated `[usage]` cost from OpenRouter crosses a threshold, with a clear message. Built on the usage totals that already exist; independent of the indicator in 2.2.

### 2.5 File mentions in prompts
Axis: **input**. `@path/to/file` in a prompt expands to the file's contents (capped, escaped) in front of the message. A fast way to point the agent at a file without a round trip, and it composes with tab completion that already resolves paths.

---

## 3. Sessions and state

Today state is three tiny files; a conversation is lost on exit. These features make a run reproducible and inspectable.

### 3.1 Named / persistent sessions
Axis: **state**. `--session NAME` to save the message history under `$XDG_STATE_HOME/myra/sessions/NAME.json` and resume it with `--resume NAME`. Independent of provider/model memory, and the natural substrate for the other session features.

### 3.2 Transcript export
Axis: **interchange**. `--transcript FILE` (or `/save FILE`) writes the conversation as newline-delimited JSON: each message, tool call, tool result, usage line and timestamp. Makes runs diffable, shareable and replayable in tests.

### 3.3 Replay / record
Axis: **testing**. A mode that records the wire traffic of a session and replays it to reproduce a bug without a live server or API key. Pairs with the existing mock-server e2e suite (`tests/test_agent.py`) and is orthogonal to transcript export, which records the logical conversation rather than the HTTP.

### 3.4 `/undo` and `/diff`
Axis: **edit safety**. Because `write` and `edit` are atomic, myra can keep the previous content of each touched file for the current turn and offer `/undo` to restore it, plus `/diff` to show what changed. Complements the permission model (see 4) without touching it.

---

## 4. Permissions and safety

### 4.1 Remembered approvals
Axis: **approval UX**. In `--permissions ask`, offer "always allow" for a specific command prefix or path for the rest of the session, so repetitive safe calls stop prompting. Orthogonal to the existing modes, which are global and stateless.

### 4.2 Declarative allow/deny rules
Axis: **policy**. A config file (e.g. `.myra/permissions`) with glob rules: allow `shell: git *`, deny `write: **/*.key`, deny `shell: rm -rf *`. Generalizes the current cwd-based check and makes `auto` usable in more places. Distinct from 4.1 (ad-hoc, in-session) because it is pre-declared and shareable with a repo.

### 4.3 Dry-run / plan mode
Axis: **preview**. `--dry-run` prints every tool call it *would* make and makes none, so a prompt can be sanity-checked before it touches the tree. Independent of permissions: it is about inspection, not authorization.

### 4.4 Path-scoped `read`
Axis: **safety**. The README notes `read` is unconfined and can exfiltrate keys. An opt-in `--read-root DIR` (or extending the permission rules to `read`) closes that hole for users who care, without changing the default.

---

## 5. Model and provider behavior

### 5.1 Model fallback chain
Axis: **resilience**. `-m` accepting a comma-separated list, or a configurable fallback when a request fails with 404/429/5xx, so a run survives one unavailable model. Orthogonal to retry/backoff, which retries the *same* endpoint.

### 5.2 Generation parameters
Axis: **model control**. `--temperature`, `--max-tokens`, `--reasoning-effort` (and remembering them per provider alongside the model). Small, and needed the moment myra is used for anything but coding.

### 5.3 Provider health / config introspection
Axis: **diagnostics**. `myra --doctor` (or `/status`) prints the resolved provider, base URL, model, key presence (not value), output cap, state directory and where the working directory check is rooted. Cheap, and removes the most common "why did it fail" questions.

### 5.4 Readline in headless pipelines
Axis: **input**. Piped input already works; add `--`-separated prompts or a `--stdin` mode that reads one prompt per line from a file, so a batch of tasks can be driven in one process without re-paying startup. Independent of the REPL.

---

## 6. Extensibility and integration

### 6.1 MCP client
Axis: **extensibility**. Speak the Model Context Protocol over stdio to configured servers, exposing their tools to the loop as extra entries in the existing `tools` array. This is the single highest-leverage feature for adopting myra alongside other tools, and it is additive: the four built-ins keep working, the tool loop is unchanged in shape. Config lives in a small JSON file under the state dir.

### 6.2 Hooks
Axis: **automation**. Run a configured shell command before/after `write`/`edit`/`shell` (e.g. format after edit, run the test on shell). Lets teams enforce style without a new tool, and reuses the `shell` machinery and permission checks.

### 6.3 `--json` output mode
Axis: **scriptability**. Emit the reply, tool calls and usage as newline-delimited JSON on stdout instead of rendered text. Makes myra usable from scripts and CI, where parsing stderr is unacceptable.

### 6.4 User-defined tools via config
Axis: **extensibility**. A JSON file declaring extra OpenAI-style tools whose implementation is a shell command with argument templating. A "poor man's MCP" that needs no protocol and no runtime. Orthogonal to 6.1 for users who only want one or two commands.

---

## 7. Distribution and onboarding

### 7.1 Package the binary
Axis: **distribution**. Homebrew tap, Debian `.deb`, a static musl build, and a GitHub release with checksums. The binary is ~135 KB and links only libc and libcurl; a static build makes it copy-and-run. Pure packaging, no code change.

### 7.2 Shell completions
Axis: **onboarding**. Generate `bash`/`zsh`/`fish` completion for the CLI flags, not just the REPL commands. Discoverability for flags like `--permissions` and `-P`.

### 7.3 Man page / `--help` examples
Axis: **documentation**. A `myra.1` man page generated from the same table as `--help`, plus a few runnable examples in the README. Small, but reduces support load.

---

## 8. Suggested first tranche

If only a few ship, these give the most value per line of C and preserve the "minimal agent" identity:

1. **Project instruction file (2.1)** — one read, huge behavioral gain.

2. **`list` + `grep` tools (1.1, 1.2)** — less shell noise, finer permissions.

3. **`@file` mentions (2.5)** — better prompts with no new tool.

4. **`--json` output (6.3)** and **transcript export (3.2)** — makes myra scriptable and testable.

5. **MCP client (6.1)** — the one feature that materially changes what the agent can do, at the cost of a new subsystem.

Items that would violate the design ethos (a web UI, a plugin VM, a database of embeddings) are intentionally absent.
