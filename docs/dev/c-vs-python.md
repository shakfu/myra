# C vs Python: speed

Measured 2026-09-23 at commit `a0e5f95`, on an Apple M1 with 16 GB. Compares `build/myra` (Release) with `scripts/agent.py` on Python 3.11.12 and 3.14.7.

## Result

C does its own work 2.5 to 30 times faster, depending on the measure. With a local model, wall time is the same within noise: 95% of a turn is spent waiting for the server.

## Measurements

### Startup

`hyperfine -N --warmup 5 -m 30 '<agent> -V'`:

| Agent       | Mean     |
|-------------|----------|
| C           | 3.5 ms   |
| Python 3.11 | 61 ms    |
| Python 3.14 | 106 ms   |

### Agent overhead, no model

Each agent ran headless (`-P local -p go`) against the `Mock` server from `tests/test_agent.py`, which replies with no inference delay. CPU time is the agent process and its children, from `getrusage(RUSAGE_CHILDREN)`. The mock runs in the parent process, so its CPU is excluded. Median of 5 runs.

| Scenario                                | C     | Python 3.11 | Python 3.14 |
|-----------------------------------------|-------|-------------|-------------|
| 40 turns, each a `read` of a 50 KB file | 40 ms | 105 ms      | 164 ms      |
| One reply of 20,000 one-token events    | 72 ms | 176 ms      | 263 ms      |

Python costs about 1.5 to 3 ms more per request and 5 to 10 µs more per streamed event.

### Live model

`/opt/llama/llama-server` build 10850 (`f114f91f9`), started with:

    llama-server -m ~/.models/Qwen3-4B-Q8_0.gguf --jinja --port 8091 --temp 0 --seed 1 \
        -c 16384 -np 1 --chat-template-kwargs '{"enable_thinking":false}'

Task, run with `-P local --permissions all -p`: "Use the write tool to create notes.txt containing the line 'alpha beta', then use the shell tool to run 'wc -w notes.txt', then tell me the word count."

Controls:

- `--temp 0`: greedy decoding, so both agents receive the same tokens.
- One fixed working directory, emptied between runs. The system prompt contains the path, so a different path changes the model's input.
- `XDG_STATE_HOME` in a scratch directory, so no user state is read or written.
- One warmup run per agent, then 5 runs, alternating C, 3.11, 3.14.

Every run made 2 requests, used 1270 tokens in and 73 out, ran 2 tools, and gave the same answer.

| Agent       | Median wall | Fastest wall | Agent CPU |
|-------------|-------------|--------------|-----------|
| C           | 10.82 s     | 10.67 s      | 52 ms     |
| Python 3.11 | 11.12 s     | 10.63 s      | 311 ms    |
| Python 3.14 | 11.03 s     | 10.74 s      | 456 ms    |

The medians differ by 2 to 3%, and the fastest runs overlap. A `cProfile` run of Python 3.11 put 10.51 s of 11 s in `socket.recv_into`, waiting for the server.

The agent CPU under a live model is 3 to 4 times what the no-model results predict. This is not explained. One hypothesis, untested: `llama-server` occupies the performance cores, so the agent runs on efficiency cores and needs more CPU time for the same work.

## When the difference matters

- Replies streamed at thousands of tokens per second, where 5 to 10 µs per event becomes visible.
- Scripts that start the agent many times: Python adds 60 to 100 ms per start.
- Machines where CPU time is limited or billed.

For interactive use with a 4B model on an M1, it does not matter. A fast cloud model shortens the wait, but Python's added cost stays in milliseconds per request.

## Other observations

- Llama 3.2 1B (`Llama-3.2-1B-Instruct-Q4_K_M.gguf`) failed every run. `llama-server` rejected its tool calls with HTTP 500 "The model produced output that does not match the expected peg-native format".
- Both agents retried that 500 four times. With greedy decoding every retry gets the same reply, so the retries only add latency.
- Python 3.14 is slower than 3.11 in every measurement here, by 45 to 75% in startup and CPU time.

## Reproducing

The benchmark scripts are not in the repository. The no-model benchmark imports `Mock` and `Api` from `tests/test_agent.py` and must run under `uv run --no-project --with pytest`. The live benchmark needs only the standard library.
