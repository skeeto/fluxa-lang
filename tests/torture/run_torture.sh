#!/usr/bin/env bash
# run_torture.sh — executed inside the torture container
# Runs test-all with the pre-compiled binary, skipping build steps.
set -euo pipefail

FLUXA="/fluxa/fluxa"
TESTS="/fluxa/tests"

echo "════════════════════════════════════════════════════════"
echo "  Fluxa Torture Test (IoT simulation)"
printf "  CPU: 0.1 core  |  RAM: 128MB  |  Swap: none\n"
echo "════════════════════════════════════════════════════════"
echo ""

# Verify binary deps are satisfied — fail fast with clear message
echo "── Checking binary dependencies ─────────────────────────"
if ! ldd "$FLUXA" > /tmp/ldd_out.txt 2>&1; then
    echo "  ERROR: ldd failed — binary may not be compatible with this container"
    cat /tmp/ldd_out.txt
    exit 1
fi
MISSING=$(grep "not found" /tmp/ldd_out.txt || true)
if [ -n "$MISSING" ]; then
    echo "  MISSING LIBS:"
    echo "$MISSING" | sed "s/^/    /"
    echo ""
    echo "  Add missing libs to Dockerfile.torture and rebuild."
    exit 1
fi
echo "  All shared libs satisfied"
echo ""

# Show resource state at start
echo "── Resource state on entry ──────────────────────────────"
cat /proc/meminfo | grep -E "MemTotal|MemAvailable|SwapTotal"
echo ""

PASS=0; FAIL=0

run_suite() {
    local name="$1"; shift
    printf "  %-52s" "$name"
    if "$@" > /tmp/torture_out.txt 2>&1; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        echo "FAIL"
        FAIL=$((FAIL+1))
        grep -E "FAIL|error" /tmp/torture_out.txt | head -3 | sed 's/^/    /'
    fi
}

cd /fluxa

# Core test suite
run_suite "core tests"          bash "$TESTS/run_tests.sh" "$FLUXA"
run_suite "suite2/block"        bash "$TESTS/suite2/s2_block.sh" --fluxa "$FLUXA"
run_suite "suite2/gc"           bash "$TESTS/suite2/s2_gc.sh" --fluxa "$FLUXA"
run_suite "suite2/dyn"          bash "$TESTS/suite2/s2_dyn.sh" --fluxa "$FLUXA"
run_suite "suite2/types_danger" bash "$TESTS/suite2/s2_types_danger.sh" --fluxa "$FLUXA"
run_suite "suite2/flxthread"    bash "$TESTS/suite2/s2_flxthread.sh" --fluxa "$FLUXA"
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true
run_suite "sprint9b/set_loop"   bash "$TESTS/sprint9b_set_in_loop.sh" --fluxa "$FLUXA"
rm -f /tmp/fluxa-*.sock /tmp/fluxa-*.lock 2>/dev/null || true
run_suite "sprint9b/explain"    bash "$TESTS/sprint9b_explain_live.sh" --fluxa "$FLUXA"
run_suite "sprint10b"           bash "$TESTS/sprint10b_core_fixes.sh"
run_suite "integration"         bash "$TESTS/integration/run_all.sh" --fluxa "$FLUXA"

echo ""
echo "────────────────────────────────────────────────────────"
echo "  Results: $PASS passed, $FAIL failed"
cat /proc/meminfo | grep "MemAvailable"

if [ $FAIL -eq 0 ]; then
    echo "  → Torture: PASS — runtime stable under IoT constraints"
    exit 0
else
    echo "  → Torture: FAIL — $FAIL suite(s) failed under resource pressure"
    exit 1
fi
