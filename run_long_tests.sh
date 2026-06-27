#!/usr/bin/env bash
#
# run_long_tests.sh
#
# Run the "long" / meaningful test suites and skip the short single-action MCP
# server tests (each of which spins up a whole game just to check one verb or
# object — e.g. test_maniac_c64's per-action tests). What runs:
#
#   mcp   — test/mcp walkthrough + multi-step puzzle tests (everything except the
#           single-action files in MCP_SKIP below).
#   bench — the whole scummvm_bench suite incl. the real full-game runs
#           (--run-real); this also covers the no-game-fixture mock/unit tests.
#   cpp   — the ScummVM C++ unit tests (make test).
#
# Usage:  ./run_long_tests.sh [group]      group = mcp | bench | cpp | all (default)
#
# Every selected group runs even if an earlier one fails; a summary is printed at
# the end and the script exits non-zero if any group failed. Assumes the tree is
# already configured/built (run build_test_merge.sh or ./configure && make first).
#
set -uo pipefail

# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------
REPO="/Users/xhardy/Personal/llm/scummvm/myuser/scummvm"
GROUP="${1:-all}"
MCP_WORKERS="${MCP_WORKERS:-8}"   # pytest-xdist workers for the MCP server tests

# Single-action MCP server test files to skip (each launches a game per action).
MCP_SKIP=(
    test_maniac_c64.py
    test_maniac_phone.py
    test_comi.py
    test_dig.py
)

# Game-data locations for the MCP server tests; other games keep their built-in
# defaults and simply skip if absent.
export MONKEY_DEMO_PATH="/Users/xhardy/Personal/llm/scummvm/myuser/monkey"
export ATLANTIS_DEMO_PATH="/Users/xhardy/Personal/llm/scummvm/games/Indy4Dem"
export MANIAC_C64_PATH="/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"

# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------
stage(){ printf '\n\033[1;36m==================== %s ====================\033[0m\n' "$*"; }
info() { printf '\033[0;32m• %s\033[0m\n' "$*"; }
warn() { printf '\033[1;31m✗ %s\033[0m\n' "$*"; }

FAILED=""

# --------------------------------------------------------------------------
# Groups — each returns the underlying test command's exit status.
# --------------------------------------------------------------------------
group_mcp() {
    cd "${REPO}/test/mcp" || return 1
    local ignores=() f
    for f in "${MCP_SKIP[@]}"; do
        ignores+=(--ignore="${f}")
    done
    ./venv/bin/python -m pytest -n "${MCP_WORKERS}" -p no:cacheprovider -q "${ignores[@]}"
}

group_bench() {
    cd "${REPO}/scummvm_bench" || return 1
    local rc
    UV_CONFIG_FILE="${PWD}/uv.toml" uv run --no-sync pytest --run-real
    rc=$?
    # The real runs write in place to tracked save fixtures; reset them so the
    # tree is left clean (mirrors build_test_merge.sh).
    git -C "${REPO}" checkout -- test/mcp/save_slots
    return "${rc}"
}

group_cpp() {
    cd "${REPO}" || return 1
    make test
}

run_group() {
    local name="$1" fn="$2"
    stage "GROUP: ${name}"
    if "${fn}"; then
        info "GROUP ${name}: PASSED"
    else
        local rc=$?
        warn "GROUP ${name}: FAILED (exit ${rc})"
        FAILED="${FAILED} ${name}"
    fi
}

# --------------------------------------------------------------------------
# Dispatch
# --------------------------------------------------------------------------
stage "run_long_tests — group=${GROUP}  mcp-workers=${MCP_WORKERS}"

case "${GROUP}" in
    all)   run_group mcp group_mcp; run_group bench group_bench; run_group cpp group_cpp ;;
    mcp)   run_group mcp group_mcp ;;
    bench) run_group bench group_bench ;;
    cpp)   run_group cpp group_cpp ;;
    *)     warn "unknown group '${GROUP}' (use: mcp | bench | cpp | all)"; exit 2 ;;
esac

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
stage "SUMMARY"
if [[ -n "${FAILED}" ]]; then
    warn "failed groups:${FAILED}"
    exit 1
fi
info "all selected groups passed"
