SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
PYTHON ?= python3
JOBS ?= $(shell nproc)

CURRENT_BUILD ?= build/bench-current
CURRENT_BENCH := $(CURRENT_BUILD)/TinyDB_bench

BENCH_OUTPUT ?=
COMPARISON_OUTPUT ?=
BENCH_PROFILE ?= standard
BENCH_SEMANTICS ?= durable
BENCH_IO_MODE ?= buffered
CACHE_MIB ?=
BASELINE_CACHE_MIB ?=
BUFFERED_CACHE_MIB ?= $(BASELINE_CACHE_MIB)
DIRECT_CACHE_MIB ?=
BENCH_ARGS ?=

CACHE_ARG = $(if $(strip $(CACHE_MIB)),--cache-mib "$(CACHE_MIB)")
BUFFERED_CACHE_ARG = $(if $(strip $(BUFFERED_CACHE_MIB)),--baseline-cache-mib "$(BUFFERED_CACHE_MIB)")
DIRECT_CACHE_ARG = $(if $(strip $(DIRECT_CACHE_MIB)),--candidate-cache-mib "$(DIRECT_CACHE_MIB)")
BENCH_OUTPUT_ARG = $(if $(strip $(BENCH_OUTPUT)),--output "$(BENCH_OUTPUT)")
COMPARISON_OUTPUT_ARG = $(if $(strip $(COMPARISON_OUTPUT)),--output "$(COMPARISON_OUTPUT)")
BENCH_COMMON_ARGS = --profile "$(BENCH_PROFILE)" --semantics "$(BENCH_SEMANTICS)"

.PHONY: help bench bench-compare bench-build

help:
	@echo "TinyDB benchmark commands"
	@echo
	@echo "  make bench          Measure the current tree"
	@echo "  make bench-compare  Compare the current tree with direct I/O"
	@echo
	@echo "Use BENCH_ARGS='--family db_bench' or BENCH_ARGS='--family cold_io' for a focused run."
	@echo "Use BENCH_PROFILE=smoke or BENCH_PROFILE=soak for portable workload scale."
	@echo "Both I/O modes use the standard 16 MiB page cache."
	@echo "make bench uses buffered I/O. Set BENCH_IO_MODE=direct to request direct I/O."
	@echo "Use CACHE_MIB=8 for an equal cache-pressure run."
	@echo "BUFFERED_CACHE_MIB and DIRECT_CACHE_MIB are optional per-mode overrides."
	@echo "The latest default result replaces its predecessor; set BENCH_OUTPUT or COMPARISON_OUTPUT to archive one."
	@echo "Override JOBS as needed."

bench: bench-build
	@$(PYTHON) bench/runner.py run "$(CURRENT_BENCH)" --io-mode "$(BENCH_IO_MODE)" $(BENCH_COMMON_ARGS) $(CACHE_ARG) $(BENCH_OUTPUT_ARG) $(BENCH_ARGS)

bench-compare: bench-build
	@$(PYTHON) bench/runner.py compare "$(CURRENT_BENCH)" "$(CURRENT_BENCH)" --baseline-io-mode buffered --candidate-io-mode direct $(BENCH_COMMON_ARGS) $(CACHE_ARG) $(BUFFERED_CACHE_ARG) $(DIRECT_CACHE_ARG) $(COMPARISON_OUTPUT_ARG) $(BENCH_ARGS)

bench-build:
	@$(CMAKE) -S bench -B "$(CURRENT_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DKVBENCH_BACKEND=tinydb -DTINYDB_ENGINE_SOURCE_DIR="$(CURDIR)"
	@$(CMAKE) --build "$(CURRENT_BUILD)" --target TinyDB_bench --parallel "$(JOBS)"
