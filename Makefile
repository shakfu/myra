# Frontend to CMake. Build dir and type can be overridden: make BUILD=out TYPE=Debug
BUILD ?= build
TYPE ?= Release
CMAKE_ARGS ?=
GENERATOR := $(if $(shell command -v ninja),-G Ninja,)

all: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD)

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(GENERATOR) -DCMAKE_BUILD_TYPE=$(TYPE) $(CMAKE_ARGS)

test: all
	ctest --test-dir $(BUILD) --output-on-failure -j 8

# Same, in a separate tree with AddressSanitizer and UBSan.
asan:
	$(MAKE) test BUILD=build-asan TYPE=Debug CMAKE_ARGS=-DAGENT_SANITIZE=ON

clean:
	rm -rf build build-asan

.PHONY: all test asan clean
