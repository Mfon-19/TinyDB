SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

CMAKE ?= cmake
PYTHON ?= python3
JOBS ?= $(shell nproc)

DIRECT_IO_REVISION ?= direct-io
DIRECT_IO_COMMIT := $(shell git rev-parse $(DIRECT_IO_REVISION))
DIRECT_IO_SHORT := $(shell git rev-parse --short=12 $(DIRECT_IO_COMMIT))

WORKTREE_ROOT ?= /tmp/tinydb-benchmark-worktrees

CURRENT_BUILD ?= build/bench-current
DIRECT_IO_BUILD ?= build/bench-direct-$(DIRECT_IO_SHORT)
DIRECT_IO_ROOT := $(WORKTREE_ROOT)/direct-io-$(DIRECT_IO_SHORT)
CURRENT_BENCH := $(CURRENT_BUILD)/TinyDB_bench
DIRECT_IO_BENCH := $(DIRECT_IO_BUILD)/TinyDB_bench

BENCH_OUTPUT ?=
COMPARISON_OUTPUT ?=
DIRECT_CACHE_MIB ?= 16
BENCH_ARGS ?=

.PHONY: help bench bench-compare bench-build bench-direct-build

help:
	@echo "TinyDB benchmark commands"
	@echo
	@echo "  make bench          Measure the current tree"
	@echo "  make bench-compare  Compare the current tree with direct I/O"
	@echo
	@echo "Use BENCH_ARGS='--family reads' or BENCH_ARGS='--filter cold' for a focused run."
	@echo "Direct I/O uses a 16 MiB cache by default; use DIRECT_CACHE_MIB=32 to override it."
	@echo "The latest default result replaces its predecessor; set BENCH_OUTPUT or COMPARISON_OUTPUT to archive one."
	@echo "Override DIRECT_IO_REVISION or JOBS as needed."

bench: bench-build
	@$(PYTHON) bench/runner.py run "$(CURRENT_BENCH)" \
		$(if $(strip $(BENCH_OUTPUT)),--output "$(BENCH_OUTPUT)") $(BENCH_ARGS)

bench-compare: bench-build bench-direct-build
	@$(PYTHON) bench/runner.py compare "$(CURRENT_BENCH)" "$(DIRECT_IO_BENCH)" \
		$(if $(strip $(DIRECT_CACHE_MIB)),--candidate-cache-mib "$(DIRECT_CACHE_MIB)") \
		$(if $(strip $(COMPARISON_OUTPUT)),--output "$(COMPARISON_OUTPUT)") $(BENCH_ARGS)

bench-build:
	@$(CMAKE) -S bench -B "$(CURRENT_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DTINYDB_ENGINE_SOURCE_DIR="$(CURDIR)"
	@$(CMAKE) --build "$(CURRENT_BUILD)" --target TinyDB_bench --parallel "$(JOBS)"

bench-direct-build: $(DIRECT_IO_ROOT)/.git
	@$(CMAKE) -S bench -B "$(DIRECT_IO_BUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DTINYDB_ENGINE_SOURCE_DIR="$(DIRECT_IO_ROOT)"
	@$(CMAKE) --build "$(DIRECT_IO_BUILD)" --target TinyDB_bench --parallel "$(JOBS)"

$(DIRECT_IO_ROOT)/.git:
	@mkdir -p "$(WORKTREE_ROOT)"
	@git worktree add --detach "$(DIRECT_IO_ROOT)" "$(DIRECT_IO_COMMIT)"
