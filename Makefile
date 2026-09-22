CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags libcjson libcurl)
LDLIBS += $(shell pkg-config --libs libcjson libcurl)

agent: agent.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: agent
	uv run --no-project --with pytest pytest -q tests

clean:
	rm -f agent

.PHONY: test clean
