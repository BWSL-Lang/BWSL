#!/bin/bash
# BWSL Regression Test Runner
# Runs the compiler on all test files and reports results

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BWSL_ROOT="$(dirname "$SCRIPT_DIR")"
BWSLC="$BWSL_ROOT/build/bwslc"
OUTPUT_DIR="$SCRIPT_DIR/output"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0
SKIPPED=0

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Check if compiler exists
if [ ! -f "$BWSLC" ]; then
    echo -e "${YELLOW}Building compiler...${NC}"
    (cd "$BWSL_ROOT" && make bwslc)
    if [ ! -f "$BWSLC" ]; then
        echo -e "${RED}Failed to build compiler${NC}"
        exit 1
    fi
fi

echo "========================================"
echo "BWSL Regression Test Suite"
echo "========================================"
echo ""

# Run each test
for test_file in "$SCRIPT_DIR"/*.bwsl; do
    test_name=$(basename "$test_file" .bwsl)

    # Skip module-only files (they need to be imported, not compiled directly)
    # Modules that start with 'module' keyword need special handling
    first_keyword=$(grep -m1 "^module\|^pipeline" "$test_file" | awk '{print $1}')
    if [ "$first_keyword" = "module" ]; then
        echo -e "[${YELLOW}SKIP${NC}] $test_name (module file)"
        ((SKIPPED++))
        continue
    fi

    # Run compiler
    compile_flags=""
    if [ "$test_name" = "inline_return_jump" ] || [ "$test_name" = "inline_return_loop" ] || [ "$test_name" = "inline_return_nested_loops" ] || [ "$test_name" = "inline_return_nested_loops_skip_break" ] || [ "$test_name" = "inline_return_outer_skip_break" ] || [ "$test_name" = "inline_return_nested_if" ] || [ "$test_name" = "inline_return_range_loop" ] || [ "$test_name" = "inline_return_count_loop" ] || [ "$test_name" = "inline_return_loop_until" ] || [ "$test_name" = "inline_return_enum" ]; then
        compile_flags="-internals"
    fi

    output=$("$BWSLC" "$test_file" -o "$OUTPUT_DIR" $compile_flags 2>&1)
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo -e "[${GREEN}PASS${NC}] $test_name"
        ((PASSED++))

        if [ "$test_name" = "inline_return_jump" ] || [ "$test_name" = "inline_return_nested_if" ] || [ "$test_name" = "inline_return_enum" ]; then
            check_file="$OUTPUT_DIR/${test_name}_pass0_vert.internals.json"
            python3 - "$check_file" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

ir = data.get("ir", "")
branch_count = sum(1 for line in ir.splitlines() if " BRANCH " in line)
if " JUMP " in ir:
    print("Unexpected inline return jump in IR")
    sys.exit(1)
if branch_count < 2:
    print(f"Expected return guard branch, found {branch_count}")
    sys.exit(1)
PY
            if [ $? -ne 0 ]; then
                echo -e "[${RED}FAIL${NC}] $test_name"
                echo "       Error: inline return jump check failed"
                ((FAILED++))
                ((PASSED--))
            fi
        fi
        if [ "$test_name" = "inline_return_loop" ] || [ "$test_name" = "inline_return_nested_loops" ] || [ "$test_name" = "inline_return_nested_loops_skip_break" ] || [ "$test_name" = "inline_return_outer_skip_break" ] || [ "$test_name" = "inline_return_range_loop" ] || [ "$test_name" = "inline_return_count_loop" ] || [ "$test_name" = "inline_return_loop_until" ]; then
            check_file="$OUTPUT_DIR/${test_name}_pass0_vert.internals.json"
            python3 - "$check_file" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

spirv = data.get("spirv_dis", "")
if "OpLogicalAnd" not in spirv:
    print("Missing return guard logical AND in SPIR-V")
    sys.exit(1)
PY
            if [ $? -ne 0 ]; then
                echo -e "[${RED}FAIL${NC}] $test_name"
                echo "       Error: inline return loop guard check failed"
                ((FAILED++))
                ((PASSED--))
            fi
        fi
    else
        echo -e "[${RED}FAIL${NC}] $test_name"
        echo "       Error: $output"
        ((FAILED++))
    fi
done

echo ""
echo "========================================"
echo "Results: $PASSED passed, $FAILED failed, $SKIPPED skipped"
echo "========================================"

# Exit with failure if any tests failed
if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
