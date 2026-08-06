SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
PYTHON ?= python3
JOBS ?= $(shell nproc)

DIRECT_IO_REVISION ?= direct-io
DIRECT_IO_COMMIT = $(shell git rev-parse $(DIRECT_IO_REVISION))
DIRECT_IO_SHORT = $(shell git rev-parse --short=12 $(DIRECT_IO_COMMIT))

WORKTREE_ROOT ?= /tmp/tinydb-benchmark-worktrees

CURRENT_BUILD ?= build/bench-current
DIRECT_IO_BUILD ?= build/bench-direct-$(DIRECT_IO_SHORT)
DIRECT_IO_ROOT = $(WORKTREE_ROOT)/direct-io-$(DIRECT_IO_SHORT)
CURRENT_BENCH := $(CURRENT_BUILD)/TinyDB_bench
DIRECT_IO_BENCH = $(DIRECT_IO_BUILD)/TinyDB_bench

BENCH_OUTPUT ?=
COMPARISON_OUTPUT ?=
BENCH_PROFILE ?= standard
BENCH_SEMANTICS ?= durable
CACHE_MIB ?=
BASELINE_CACHE_MIB ?=
DIRECT_CACHE_MIB ?=
BENCH_ARGS ?=

MICROBENCH_BUILD ?= build/microbench
MICROBENCH := $(MICROBENCH_BUILD)/TinyDB_microbench
MICROBENCH_ARGS ?= --benchmark_repetitions=10 --benchmark_min_time=0.1s \
	--benchmark_enable_random_interleaving=true --benchmark_display_aggregates_only=true

CACHE_ARG = $(if $(strip $(CACHE_MIB)),--cache-mib "$(CACHE_MIB)")
BASELINE_CACHE_ARG = $(if $(strip $(BASELINE_CACHE_MIB)),--baseline-cache-mib "$(BASELINE_CACHE_MIB)")
DIRECT_CACHE_ARG = $(if $(strip $(DIRECT_CACHE_MIB)),--candidate-cache-mib "$(DIRECT_CACHE_MIB)")
BENCH_OUTPUT_ARG = $(if $(strip $(BENCH_OUTPUT)),--output "$(BENCH_OUTPUT)")
COMPARISON_OUTPUT_ARG = $(if $(strip $(COMPARISON_OUTPUT)),--output "$(COMPARISON_OUTPUT)")
BENCH_COMMON_ARGS = --profile "$(BENCH_PROFILE)" --semantics "$(BENCH_SEMANTICS)"

.PHONY: help bench bench-compare bench-build bench-direct-build microbench microbench-build

help:
	@echo "TinyDB benchmark commands"
	@echo
	@echo "  make bench          Measure the current tree"
	@echo "  make bench-compare  Compare the current tree with direct I/O"
	@echo "  make microbench     Measure in-memory components"
	@echo
	@echo "Use BENCH_ARGS='--family db_bench' or BENCH_ARGS='--family cold_io' for a focused run."
	@echo "Use BENCH_PROFILE=smoke or BENCH_PROFILE=soak for portable workload scale."
	@echo "Both engines use the standard 16 MiB page cache."
	@echo "Use CACHE_MIB=8 for an equal cache-pressure run."
	@echo "BASELINE_CACHE_MIB and DIRECT_CACHE_MIB are optional per-engine overrides."
	@echo "The latest default result replaces its predecessor; set BENCH_OUTPUT or COMPARISON_OUTPUT to archive one."
	@echo "Override DIRECT_IO_REVISION or JOBS as needed."
	@echo "Use MICROBENCH_ARGS='--benchmark_filter=Overflow' for a focused microbenchmark run."

bench: bench-build
	@$(PYTHON) bench/runner.py run "$(CURRENT_BENCH)" $(BENCH_COMMON_ARGS) $(CACHE_ARG) $(BENCH_OUTPUT_ARG) $(BENCH_ARGS)

bench-compare: bench-build bench-direct-build
	@$(PYTHON) bench/runner.py compare "$(CURRENT_BENCH)" "$(DIRECT_IO_BENCH)" $(BENCH_COMMON_ARGS) $(CACHE_ARG) $(BASELINE_CACHE_ARG) $(DIRECT_CACHE_ARG) $(COMPARISON_OUTPUT_ARG) $(BENCH_ARGS)

bench-build:
	@$(CMAKE) -S bench -B "$(CURRENT_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DKVBENCH_BACKEND=tinydb -DTINYDB_ENGINE_SOURCE_DIR="$(CURDIR)"
	@$(CMAKE) --build "$(CURRENT_BUILD)" --target TinyDB_bench --parallel "$(JOBS)"

bench-direct-build:
	@mkdir -p "$(WORKTREE_ROOT)"
	@if [[ ! -e "$(DIRECT_IO_ROOT)/.git" ]]; then \
		git worktree add --detach "$(DIRECT_IO_ROOT)" "$(DIRECT_IO_COMMIT)"; \
	fi
	@$(CMAKE) -S bench -B "$(DIRECT_IO_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DKVBENCH_BACKEND=tinydb -DTINYDB_ENGINE_SOURCE_DIR="$(DIRECT_IO_ROOT)"
	@$(CMAKE) --build "$(DIRECT_IO_BUILD)" --target TinyDB_bench --parallel "$(JOBS)"

microbench: microbench-build
	@"$(MICROBENCH)" $(MICROBENCH_ARGS)

microbench-build:
	@$(CMAKE) -S . -B "$(MICROBENCH_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTINYDB_BUILD_MICROBENCHMARKS=ON
	@$(CMAKE) --build "$(MICROBENCH_BUILD)" --target TinyDB_microbench --parallel "$(JOBS)"
