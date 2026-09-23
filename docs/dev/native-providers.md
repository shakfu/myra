# Native OpenAI and Anthropic providers

Decided 2026-09-23 at commit `900f877`. Considered adding `openai` (Responses API) and `anthropic` (Messages API) as providers with their native wire protocols.

## Decision

Not now. Keep `openrouter` and `local`. OpenRouter already serves the latest OpenAI and Anthropic models.

## What native protocols would add

| Gain | Through OpenRouter |
|------|--------------------|
| Latest models | Yes |
| Prompt caching | Yes: top-level `cache_control` for Claude; OpenAI caches automatically |
| Reasoning kept across tool calls | Probably: myra replays `reasoning_details`, including signatures and encrypted entries. Unverified |
| Latency | One extra hop; 95% of a turn is server time (see `c-vs-python.md`) |
| Cost | OpenRouter's credit-purchase fee, about 5.5% (unverified) |
| Context editing, compaction, effort, server tools | No; myra uses none of them |
| Direct data terms, no intermediary | No |

## Cost

- Four adapters: two wire formats in C and Python. Each needs request translation, an SSE event parser, usage and stop-reason mapping, and paginated `/models`.
- Estimated +500-600 lines of C, +350-400 of Python, +300 of test harness: about 40% on a 2,400-line core. The mock in `tests/test_agent.py` would speak three protocols.
- History is stored in chat-completions shape. Responses and Messages have state that translation can lose: encrypted reasoning items, thinking signatures, preserved-thinking rules. The alternative is three history shapes.
- Both APIs made breaking changes through 2025-2026: thinking config, forced `tool_choice` rejected on some models, edited history rejected. Each becomes a fix in two languages. OpenRouter handles these today.

## Revisit when

1. Policy requires direct vendor terms or no intermediary.
2. A measured quality gap: the same tool-loop tasks score worse through OpenRouter, or reasoning is dropped between tool calls.
3. A native-only feature fixes a real bug, e.g. server-side context editing for the preserved-thinking issue in `TODO.md`.

If built, start with Anthropic Messages. Its shape is closest to myra's, and thinking signatures are where translation is most likely to lose data. Build C and Python together, since `e2e-py` runs the same suite against both.
