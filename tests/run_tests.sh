#!/bin/bash
# BWSL Regression Test Runner
# Runs the compiler on all test files and reports results

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BWSL_ROOT="$(dirname "$SCRIPT_DIR")"
BWSLC="$BWSL_ROOT/build/bwslc"
OUTPUT_DIR="$SCRIPT_DIR/output"
GOLDEN_DIR="$SCRIPT_DIR/golden"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0
SKIPPED=0
METAL_PASSED=0
METAL_FAILED=0
GOLDEN_PASSED=0
GOLDEN_FAILED=0
GOLDEN_UPDATED=0

# Options
METAL_VALIDATION=false
UPDATE_GOLDEN=false
VERBOSE=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --metal|-m)
            METAL_VALIDATION=true
            shift
            ;;
        --update-golden|-u)
            UPDATE_GOLDEN=true
            METAL_VALIDATION=true  # Need Metal output to update golden files
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --metal, -m         Enable Metal shader validation (macOS only)"
            echo "  --update-golden, -u Update golden files with current Metal output"
            echo "  --verbose, -v       Show detailed output"
            echo "  --help, -h          Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check if Metal validation is available (macOS only)
METAL_AVAILABLE=false
if [[ "$OSTYPE" == "darwin"* ]] && command -v xcrun &> /dev/null; then
    METAL_AVAILABLE=true
fi

if $METAL_VALIDATION && ! $METAL_AVAILABLE; then
    echo -e "${YELLOW}Warning: Metal validation requested but not available (requires macOS with Xcode)${NC}"
    METAL_VALIDATION=false
fi

# Create output directories
mkdir -p "$OUTPUT_DIR"
mkdir -p "$GOLDEN_DIR"

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
if $METAL_VALIDATION; then
    echo -e "Metal validation: ${GREEN}enabled${NC}"
fi
if $UPDATE_GOLDEN; then
    echo -e "Mode: ${BLUE}updating golden files${NC}"
fi
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

    # Find associated config file
    config_flag=""
    # 1. Check for exact match: <test_name>.rcfg
    if [ -f "$SCRIPT_DIR/${test_name}.rcfg" ]; then
        config_flag="-config $SCRIPT_DIR/${test_name}.rcfg"
    else
        # 2. Check for prefix match: <prefix>_test.rcfg (e.g., bug_test.rcfg for bug_*)
        prefix=$(echo "$test_name" | cut -d'_' -f1)
        if [ -f "$SCRIPT_DIR/${prefix}_test.rcfg" ]; then
            config_flag="-config $SCRIPT_DIR/${prefix}_test.rcfg"
        fi
    fi

    # Run compiler
    compile_flags=""
    if [ "$test_name" = "inline_return_jump" ] || [ "$test_name" = "inline_return_loop" ] || [ "$test_name" = "inline_return_nested_loops" ] || [ "$test_name" = "inline_return_nested_loops_skip_break" ] || [ "$test_name" = "inline_return_outer_skip_break" ] || [ "$test_name" = "inline_return_nested_if" ] || [ "$test_name" = "inline_return_range_loop" ] || [ "$test_name" = "inline_return_count_loop" ] || [ "$test_name" = "inline_return_loop_until" ] || [ "$test_name" = "inline_return_enum" ] || [ "$test_name" = "inline_return_struct" ]; then
        compile_flags="-internals"
    fi

    # Add Metal output flag if validation is enabled
    if $METAL_VALIDATION; then
        compile_flags="$compile_flags -metal"
    fi

    output=$("$BWSLC" "$test_file" -o "$OUTPUT_DIR" $config_flag $compile_flags 2>&1)
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        if [ -n "$config_flag" ]; then
            config_name=$(basename $(echo "$config_flag" | awk '{print $2}'))
            echo -e "[${GREEN}PASS${NC}] $test_name (config: $config_name)"
        else
            echo -e "[${GREEN}PASS${NC}] $test_name"
        fi
        ((PASSED++))

        if [ "$test_name" = "inline_return_jump" ] || [ "$test_name" = "inline_return_nested_if" ] || [ "$test_name" = "inline_return_enum" ] || [ "$test_name" = "inline_return_struct" ]; then
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

        # Metal validation and golden file testing
        if $METAL_VALIDATION; then
            # Find all Metal files generated for this test
            metal_files=$(find "$OUTPUT_DIR" -name "${test_name}*.metal" -o -name "${test_name}.metal" 2>/dev/null | sort)

            for metal_file in $metal_files; do
                metal_basename=$(basename "$metal_file")
                golden_file="$GOLDEN_DIR/$metal_basename"

                # Step 1: Validate Metal compiles
                metal_output=$(xcrun -sdk macosx metal -c "$metal_file" -o /dev/null 2>&1)
                metal_exit=$?

                if [ $metal_exit -ne 0 ]; then
                    echo -e "       ${RED}Metal FAIL${NC}: $metal_basename"
                    if $VERBOSE; then
                        echo "$metal_output" | head -10 | sed 's/^/         /'
                    fi
                    ((METAL_FAILED++))
                    continue
                fi
                ((METAL_PASSED++))

                # Step 2: Golden file comparison/update
                if $UPDATE_GOLDEN; then
                    # Update mode: copy current output to golden
                    cp "$metal_file" "$golden_file"
                    echo -e "       ${BLUE}Golden updated${NC}: $metal_basename"
                    ((GOLDEN_UPDATED++))
                elif [ -f "$golden_file" ]; then
                    # Compare mode: diff against golden
                    if diff -q "$metal_file" "$golden_file" > /dev/null 2>&1; then
                        if $VERBOSE; then
                            echo -e "       ${GREEN}Golden match${NC}: $metal_basename"
                        fi
                        ((GOLDEN_PASSED++))
                    else
                        echo -e "       ${RED}Golden DIFF${NC}: $metal_basename"
                        if $VERBOSE; then
                            diff -u "$golden_file" "$metal_file" | head -20 | sed 's/^/         /'
                        fi
                        ((GOLDEN_FAILED++))
                    fi
                fi
            done
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
if $METAL_VALIDATION; then
    echo "Metal:   $METAL_PASSED compiled, $METAL_FAILED failed"
    if $UPDATE_GOLDEN; then
        echo "Golden:  $GOLDEN_UPDATED files updated"
    elif [ $((GOLDEN_PASSED + GOLDEN_FAILED)) -gt 0 ]; then
        echo "Golden:  $GOLDEN_PASSED matched, $GOLDEN_FAILED differ"
    fi
fi
echo "========================================"

# Exit with failure if any tests failed
if [ $FAILED -gt 0 ] || [ $METAL_FAILED -gt 0 ] || [ $GOLDEN_FAILED -gt 0 ]; then
    exit 1
fi
exit 0
