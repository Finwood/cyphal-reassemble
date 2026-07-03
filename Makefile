# cyphal-reassemble — convenience wrapper around CMake

BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc

.PHONY: all build configure test clean reconfigure help

all: build

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	cmake -B $(BUILD_DIR) $(CMAKE_FLAGS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

reconfigure:
	rm -f $(BUILD_DIR)/CMakeCache.txt
	$(MAKE) configure

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  make            Configure (if needed) and build (default)"
	@echo "  make build      Same as default"
	@echo "  make test       Build and run the test suite"
	@echo "  make configure  Run CMake configure only"
	@echo "  make reconfigure Force a fresh CMake configure"
	@echo "  make clean      Remove the build directory"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)    e.g. make BUILD_TYPE=Debug"
	@echo "  JOBS=$(JOBS)"
	@echo "  CMAKE_FLAGS=$(CMAKE_FLAGS)"
