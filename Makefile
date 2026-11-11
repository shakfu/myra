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

clean:
	rm -rf build build-asan build-ui
	rm -f myra

.PHONY: all test asan test-ui ui lint install install-ui clean

# cmake writes CMakeCache.txt before it fails; without this a failed configure
# leaves a cache behind and the next make skips configure and reports a missing Makefile.
.DELETE_ON_ERROR:
