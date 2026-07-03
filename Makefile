# cyphal-reassemble — convenience wrapper around CMake

BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc

.PHONY: all build configure test test-unit test-golden python-test wheel-bundle wheel-build wheel-test clean reconfigure help

all: build

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	cmake -B $(BUILD_DIR) $(CMAKE_FLAGS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

test-unit: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -E Golden

test-golden: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R Golden

python-test:
	uv run pytest python_tests/ -v

wheel-bundle:
	./scripts/prepare_wheel_bundle.sh

wheel-build:
	./scripts/build_wheel.sh

wheel-test: wheel-bundle
	uv run pytest python_tests/ -v

reconfigure:
	rm -f $(BUILD_DIR)/CMakeCache.txt
	$(MAKE) configure

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  make            Configure (if needed) and build (default)"
	@echo "  make build      Same as default"
	@echo "  make test       Build and run the full test suite"
	@echo "  make test-unit  Build and run unit tests only"
	@echo "  make test-golden Build and run golden parity tests only"
	@echo "  make python-test  Run Python wrapper tests (requires uv sync + C++ binary)"
	@echo "  make wheel-bundle Stage auditwheel-repaired binary into py/cyphal_reassemble/_bin/"
	@echo "  make wheel-build  Build py3-none-manylinux platform wheel into dist/"
	@echo "  make wheel-test   Run Python tests against staged _bin/ bundle"
	@echo "  make configure  Run CMake configure only"
	@echo "  make reconfigure Force a fresh CMake configure"
	@echo "  make clean      Remove the build directory"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)    e.g. make BUILD_TYPE=Debug"
	@echo "  JOBS=$(JOBS)"
	@echo "  CMAKE_FLAGS=$(CMAKE_FLAGS)"
