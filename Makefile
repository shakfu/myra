# Frontend to CMake. Build dir and type can be overridden: make BUILD=out TYPE=Debug
BUILD ?= build
TYPE ?= Release
PREFIX ?= $(HOME)/.local
CMAKE_ARGS ?=
LINK ?= myra # symlink to the built binary; empty to skip
GENERATOR := $(if $(shell command -v ninja),-G Ninja,)

all: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD)
	$(if $(strip $(LINK)),ln -sfn $(BUILD)/myra $(strip $(LINK)))

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(GENERATOR) -DCMAKE_BUILD_TYPE=$(TYPE) $(CMAKE_ARGS)

test: all
	ctest --test-dir $(BUILD) --output-on-failure -j 8

install: all
	cmake --install $(BUILD) --prefix $(PREFIX) --strip

# Same, in a separate tree with AddressSanitizer and UBSan.
asan:
	$(MAKE) test BUILD=build-asan TYPE=Debug CMAKE_ARGS=-DMYRA_SANITIZE=ON LINK=

# The CUI front end in ui/, with every test, in its own tree: ./build-ui/ui/myra-ui
test-ui:
	$(MAKE) test BUILD=build-ui CMAKE_ARGS=-DMYRA_UI=ON LINK=

# Build and run it, without tests: make ui ARGS='-P local'
ui:
	$(MAKE) all BUILD=build-ui CMAKE_ARGS=-DMYRA_UI=ON LINK=
	./build-ui/ui/myra-ui $(ARGS)

# myra and myra-ui, from build-ui, to PREFIX/bin.
install-ui:
	$(MAKE) install BUILD=build-ui CMAKE_ARGS=-DMYRA_UI=ON LINK=

# clang's static analyzer on our C, and ruff (pinned: new versions add rules) on the Python.
lint:
	for f in src/*.c; do \
	  clang --analyze -Xclang -analyzer-werror -std=c11 -D_DEFAULT_SOURCE -DMYRA_VERSION='""' \
	    -Isrc -Ivendor/cjson -Ivendor/linenoise/include -o /dev/null $$f || exit 1; \
	done
	for f in ui/*.c; do \
	  clang --analyze -Xclang -analyzer-werror -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -DMYRA_VERSION='""' \
	    -Isrc -Iui -Ivendor/cjson -Ivendor/cui -o /dev/null $$f || exit 1; \
	done
	uvx ruff@0.16.8 check scripts tests ui/tests

# Replace vendor/cui/ with release VERSION of CUI. Files land only if all three
# download and the header names VERSION; review with git diff, then make test-ui.
vendor-cui:
	@echo "$(VERSION)" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || { echo "usage: make vendor-cui VERSION=1.x.y"; exit 2; }
	tmp=$$(mktemp -d) && trap 'rm -rf "$$tmp"' EXIT && \
	rel=https://github.com/yappologistic/CUI/releases/download/v$(VERSION) && \
	curl -fsSL -o "$$tmp/cui.h" "$$rel/cui.h" && curl -fsSL -o "$$tmp/cui.c" "$$rel/cui.c" && \
	curl -fsSL -o "$$tmp/LICENSE" https://raw.githubusercontent.com/yappologistic/CUI/v$(VERSION)/LICENSE && \
	for f in cui.h cui.c; do grep -q '^/\* CUI $(VERSION),' "$$tmp/$$f" || { echo "$$f is not CUI $(VERSION)"; exit 1; }; done && \
	mv "$$tmp/cui.h" "$$tmp/cui.c" "$$tmp/LICENSE" vendor/cui/
	sed -i.bak -E 's/CUI [0-9]+\.[0-9]+\.[0-9]+ \(MIT/CUI $(VERSION) (MIT/' README.md && rm README.md.bak
	git diff --stat -- vendor/cui README.md
	@echo "vendor-cui: add a CHANGELOG entry, then run make test-ui and make lint"

clean:
	rm -rf build build-asan build-ui
	rm -f myra

.PHONY: all test asan test-ui ui lint install install-ui vendor-cui clean

# cmake writes CMakeCache.txt before it fails; without this a failed configure
# leaves a cache behind and the next make skips configure and reports a missing Makefile.
.DELETE_ON_ERROR:
