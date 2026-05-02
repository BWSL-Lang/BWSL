#!/bin/bash
set -e

echo "Building BWSL Parser..."

CXX="${CXX:-clang++}"

# Point to SPIRV-Headers in vendor folder
SPIRV_INCLUDE="../vendor/SPIRV-Headers/include"

# Check if headers exist
if [ ! -f "$SPIRV_INCLUDE/spirv/1.2/GLSL.std.450.h" ]; then
    echo "Error: SPIRV headers not found at $SPIRV_INCLUDE"
    echo "Clone them with: cd ../vendor && git clone https://github.com/KhronosGroup/SPIRV-Headers.git"
    exit 1
fi

FLAGS="-std=c++20 -g -I. -I.. -I../core -I../core/middleware -I../phases/lexing -I../phases/parser -I../phases/evaluation -I../phases/ir_generation -I../phases/ir_lowering -I../phases/control_flow -I../phases/ssa -I../phases/backends/spirv -I../phases/backends/gles -I$SPIRV_INCLUDE"

SOURCES=(
    "../phases/lexing/bwsl_lexer.cpp"
    "../phases/parser/bwsl_parser_soa.cpp"
    "../phases/evaluation/bwsl_eval_soa.cpp"
    "../core/bwsl_custom_type_registry.cpp"
    "../phases/evaluation/bwsl_eval_cache.cpp"

)

OUTPUT="bwsl_parser_test"

echo "Compiling: ${SOURCES[@]}"
echo "Using SPIRV headers from: $SPIRV_INCLUDE"
$CXX $FLAGS "${SOURCES[@]}" -o "$OUTPUT"

echo "✓ Build complete: $OUTPUT"
