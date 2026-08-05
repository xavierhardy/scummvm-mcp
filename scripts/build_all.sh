#!/bin/bash
#
# Full local verification and build for the ScummVM MCP fork:
#
#     ./scripts/build_all.sh
#
# The C++ engine comes first — everything after it depends on the ./scummvm
# binary this build produces:
#
#   1. configure, enabling exactly the engines that have an MCP bridge
#   2. make — build the engine
#   3. make test — ScummVM's own C++ unit tests (CxxTest), which cover the
#      engine-independent MCP helpers
#   4. Python auto-format + lint + type-check (ruff, ty) over both Python trees
#   5. the MCP server integration tests (test/mcp) and the MCP bench tests
#      (scummvm_bench) — both launch the binary built in step 2
#
# Every step runs even after an earlier one fails, so one invocation reports
# every problem. Exit status is non-zero if any step FAILED.
#
# Game data is not in the repo: suites with no data configured in
# game_paths.local.toml SKIP themselves rather than failing, so a clean
# checkout still reports green here.
#
# Env:
#   SKIP_CONFIGURE=1   reuse the existing config.mk instead of re-running configure
#   RUN_REAL=1         also run the tests that drive a real ScummVM (slow)
#   JOBS=N             make parallelism (default: nproc)
#
set -uo pipefail
cd "$(dirname "$0")/.."

REPO_ROOT=$PWD
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
BENCH="$REPO_ROOT/scummvm_bench"

# The engines with an MCP bridge (engines/*/mcp.cpp). scumm-7-8 is a separate
# configure sub-engine covering V7/V8 (FT, Dig, COMI), which the SCUMM bridge
# supports and the integration tests exercise.
ENGINES=(scumm scumm-7-8 sword1 sky queen gob)

STEPS=()
FAILED=0

if [ -t 1 ]; then
    c_bold=$'\033[1m'; c_red=$'\033[31m'; c_grn=$'\033[32m'; c_yel=$'\033[33m'; c_off=$'\033[0m'
else
    c_bold=; c_red=; c_grn=; c_yel=; c_off=
fi

record() { STEPS+=("$1"$'\t'"$2"$'\t'"${3:-}"); }

skip() {
    printf '%s\n\n' "${c_yel}== SKIP: $1${c_off} — $2"
    record "$1" SKIP "$2"
}

run() {
    local name=$1; shift
    printf '%s\n' "${c_bold}== $name${c_off}"
    local start=$SECONDS
    "$@"
    local rc=$? d=$((SECONDS - start))
    if [ "$rc" -eq 0 ]; then
        printf '%s\n\n' "${c_grn}-- $name OK${c_off} (${d}s)"
        record "$name" OK "${d}s"
    else
        printf '%s\n\n' "${c_red}-- $name FAILED (exit $rc)${c_off} (${d}s)"
        record "$name" FAILED "exit $rc after ${d}s"
        FAILED=1
    fi
}

printf '%s\n' "${c_bold}### build_all: scummvm-mcp${c_off}  $(date '+%Y-%m-%d %H:%M:%S')"
printf '%s\n\n' "engines=${ENGINES[*]}  jobs=$JOBS"

# ------------------------------------------------------- python toolchain ---
# Both Python trees (test/mcp and scummvm_bench) are ONE project rooted at
# scummvm_bench/pyproject.toml — one venv, one config, testpaths covering both.
uv_sync() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    uv sync
}
# ruff/ty only ever apply to that one project. test/mcp is a *sibling* of the
# pyproject, and ruff/ty discover config by walking up from each file, so that
# tree needs the config passed explicitly.
py_format() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    uv run --no-sync ruff format .
}
py_lint() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    uv run --no-sync ruff check . \
        && uv run --no-sync ruff check --config "$BENCH/pyproject.toml" ../test/mcp
}
py_types() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    uv run --no-sync ty check \
        && uv run --no-sync ty check --project "$BENCH" ../test/mcp
}
# -------------------------------------------------------------- configure ---
do_configure() {
    local args=(--disable-all-engines)
    local e
    for e in "${ENGINES[@]}"; do args+=("--enable-engine=$e"); done
    printf 'configure %s\n' "${args[*]}"
    ./configure "${args[@]}"
}
if [ -n "${SKIP_CONFIGURE:-}" ] && [ -f config.mk ]; then
    skip "configure" "SKIP_CONFIGURE set and config.mk present"
else
    run "configure (${#ENGINES[@]} MCP engines)" do_configure
fi

# ------------------------------------------------------------------- make ---
run "make -j$JOBS" make -j"$JOBS"

# ------------------------------------------------- ScummVM C++ unit tests ---
# cxxtestgen is invoked as `python`; Debian only ships `python3`, so provide a
# venv that supplies the unversioned name.
cpp_tests() {
    if ! command -v python >/dev/null 2>&1; then
        [ -d .venv ] || uv venv .venv || return 1
        PATH="$REPO_ROOT/.venv/bin:$PATH" make test
    else
        make test
    fi
}
run "make test (C++ unit tests)" cpp_tests

# ------------------------------------------------------- python toolchain ---
# Only after the engine builds and its own tests pass: the Python suites launch
# the ./scummvm binary this build just produced, so there is no point running
# them against a stale or broken one.
run "python env (uv sync)" uv_sync
cd "$REPO_ROOT"
run "python format (ruff)" py_format
run "python lint (ruff)"   py_lint
run "python types (ty)"    py_types
cd "$REPO_ROOT"

# ----------------------------------------------- MCP server + bench tests ---
# One xdist pool over both trees. Games with no data folder configured skip.
py_tests() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    local args=(--no-sync pytest -n auto)
    [ -n "${RUN_REAL:-}" ] && args+=(--run-real)
    uv run "${args[@]}" -o testpaths=../test/mcp
}
py_bench_tests() {
    cd "$BENCH" || return 1
    export UV_CONFIG_FILE="$BENCH/uv.toml"
    local args=(--no-sync pytest -n auto)
    [ -n "${RUN_REAL:-}" ] && args+=(--run-real)
    uv run "${args[@]}" -o testpaths=tests
}
run "MCP server tests (test/mcp)"   py_tests
run "MCP bench tests (scummvm_bench)" py_bench_tests
cd "$REPO_ROOT"

# ---------------------------------------------------------------- summary ---
printf '%s\n' "${c_bold}### summary: scummvm-mcp${c_off}"
for s in "${STEPS[@]}"; do
    IFS=$'\t' read -r name result detail <<<"$s"
    case $result in
        OK)   colour=$c_grn ;;
        SKIP) colour=$c_yel ;;
        *)    colour=$c_red ;;
    esac
    printf '  %s%-6s%s %-34s %s\n' "$colour" "$result" "$c_off" "$name" "$detail"
done

if [ "$FAILED" -ne 0 ]; then
    printf '\n%s\n' "${c_red}${c_bold}scummvm-mcp: FAILED${c_off}"
    exit 1
fi
printf '\n%s\n' "${c_grn}${c_bold}scummvm-mcp: OK${c_off}"
