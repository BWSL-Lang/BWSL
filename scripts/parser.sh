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

FLAGS="-std=c++17 -g -I. -I.. -I$SPIRV_INCLUDE"

SOURCES=(
    "../bwsl_lexer.cpp"
    "../bwsl_parser.cpp"
    "../bwsl_eval.cpp"
    "../bwsl_custom_type_registry.cpp"
    "../bwsl_eval_cache.cpp"

)

OUTPUT="bwsl_parser_test"

echo "Compiling: ${SOURCES[@]}"
echo "Using SPIRV headers from: $SPIRV_INCLUDE"
$CXX $FLAGS "${SOURCES[@]}" -o "$OUTPUT"

echo "✓ Build complete: $OUTPUT"