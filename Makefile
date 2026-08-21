# Thin wrapper so the common operations are one word each.
# Everything here just calls CMake; there is no second build system.

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build configure test bench replbench replay cluster election clean format format-check help

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: build
	./$(BUILD_DIR)/engine_bench --warmup 100000 --commands 1000000

replay: build
	./$(BUILD_DIR)/replay scripts/sample_orders.txt

replbench: build
	./$(BUILD_DIR)/replication_bench --commands 20000

cluster: build
	./scripts/cluster_up.sh scripts/cluster.conf $(BUILD_DIR)

election: build
	./scripts/election_demo.sh ./$(BUILD_DIR)/node

format:
	@find include src tests apps bench -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

format-check:
	@find include src tests apps bench -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make build         configure and compile"
	@echo "make test          build and run the full ctest suite"
	@echo "make bench         run the single-node baseline benchmark"
	@echo "make replay        run the sample order file through the replay driver"
	@echo "make replbench     sync vs async replication benchmark"
	@echo "make cluster       launch a local 3-node cluster"
	@echo "make election      elect a leader, kill it, watch failover"
	@echo "make format        apply clang-format in place"
	@echo "make format-check  fail if anything is unformatted"
	@echo "make clean         remove the build directory"
