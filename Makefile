# Frontend to CMake. Build dir and type can be overridden: make BUILD=out TYPE=Debug
BUILD ?= build
TYPE ?= Release
PREFIX ?= /usr/local
CMAKE_ARGS ?=
LINK ?= agent # symlink to the built binary; empty to skip
GENERATOR := $(if $(shell command -v ninja),-G Ninja,)

all: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD)
	$(if $(strip $(LINK)),ln -sfn $(BUILD)/agent $(strip $(LINK)))

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(GENERATOR) -DCMAKE_BUILD_TYPE=$(TYPE) $(CMAKE_ARGS)

test: all
	ctest --test-dir $(BUILD) --output-on-failure -j 8

install: all
	cmake --install $(BUILD) --prefix $(PREFIX)

# Same, in a separate tree with AddressSanitizer and UBSan.
asan:
	$(MAKE) test BUILD=build-asan TYPE=Debug CMAKE_ARGS=-DAGENT_SANITIZE=ON LINK=

clean:
	rm -rf build build-asan
	rm -f agent

.PHONY: all test asan install clean
