BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc

.PHONY: all build configure test clean

all: build

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	cmake -B $(BUILD_DIR) $(CMAKE_FLAGS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)