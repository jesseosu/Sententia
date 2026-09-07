# Thin wrapper so the common operations are one word each.
# Everything here just calls CMake; there is no second build system.

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build configure test bench replbench replay cluster election mutants flake asan clean format format-check help

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

# Prove the tests can fail. Breaks the code in 20 named ways and asserts
# a specific test notices each one. A survivor means an invariant nothing
# is guarding.
mutants:
	./scripts/mutation_check.py --jobs $(JOBS)

# Prove the tests are not bounded by the environment. Runs the suite
# repeatedly and fails if any run differs.
flake: build
	./scripts/flake_check.sh 10 $(BUILD_DIR)

# Assertions cannot see memory errors. This can.
asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSENTENTIA_SANITIZE=ON
	cmake --build build-asan -j $(JOBS)
	ctest --test-dir build-asan --output-on-failure

format:
	@find include src tests apps bench -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

format-check:
	@find include src tests apps bench -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

clean:
	rm -rf $(BUILD_DIR) build-mutants build-asan

help:
	@echo "make build         configure and compile"
	@echo "make test          build and run the full ctest suite"
	@echo "make bench         run the single-node baseline benchmark"
	@echo "make replay        run the sample order file through the replay driver"
	@echo "make replbench     sync vs async replication benchmark"
	@echo "make cluster       launch a local 3-node cluster"
	@echo "make election      elect a leader, kill it, watch failover"
	@echo "make mutants       break the code 20 ways, assert the tests notice"
	@echo "make flake         run the suite 10 times, fail on any difference"
	@echo "make asan          build and test under ASan and UBSan"
	@echo "make format        apply clang-format in place"
	@echo "make format-check  fail if anything is unformatted"
	@echo "make clean         remove the build directory"
