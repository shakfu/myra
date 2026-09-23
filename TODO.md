# TODO

## Critical

## High

- Check whether context-full recovery breaks on Fable 5.1 and Opus 5.5 via OpenRouter. `drop_old_tool_output` edits earlier tool results; those models reject edited history for accounts created on or after 2026-08-31 if thinking blocks are forwarded. Unverified.

## Medium

- Verify the `reasoning_details` round-trip on OpenRouter. Run a multi-step tool task with a GPT-5.x and a Claude model; confirm replayed reasoning is accepted and used. A failure is a trigger in `docs/dev/native-providers.md`.

## Low
