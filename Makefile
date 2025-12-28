# BWSL Compiler Build System
# ============================
#
# Targets:
#   make bwslc          - Build the CLI compiler (default)
#   make bwslc-debug    - Build CLI compiler with debug symbols
#   make wasm           - Build WebAssembly module
#   make wasm-debug     - Build WASM with debug info
#   make clean          - Remove all build artifacts
#   make test           - Run regression tests (TODO)
#
# Prerequisites:
#   - clang++ with C++20 support
#   - Emscripten SDK for WASM builds (emcc in PATH)

CXX = clang++
CXXFLAGS = -O3 -march=native -std=c++20 -Wall -Wextra
CXXFLAGS_DEBUG = -g -O0 -std=c++20 -Wall -Wextra

# SPIRV-Cross integration for cross-compilation
SPIRV_CROSS_DIR = vendor/SPIRV-Cross
SPIRV_CROSS_INCLUDES = -I$(SPIRV_CROSS_DIR)
SPIRV_CROSS_WRAPPER = tools/spirv_cross_wrapper.cpp
SPIRV_CROSS_FLAGS = -DUSE_SPIRV_CROSS_LIB

# Source paths
BWSLC_SRC = tools/bwslc.cpp

# Output directory
BUILD_DIR = build

# ============================================================================
# Native CLI Compiler
# ============================================================================

.PHONY: all bwslc bwslc-debug clean wasm wasm-debug test install

all: bwslc

bwslc: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) -I. \
		-o $(BUILD_DIR)/bwslc $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
	@echo "Built: $(BUILD_DIR)/bwslc"

bwslc-debug: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_DEBUG) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) -I. \
		-o $(BUILD_DIR)/bwslc-debug $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
	@echo "Built: $(BUILD_DIR)/bwslc-debug"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ============================================================================
# WebAssembly Build
# ============================================================================

WASM_DIR = $(BUILD_DIR)/wasm
WASM_SOURCE = tools/bwsl_wasm.cpp

# Emscripten flags
EMCC = emcc
WASM_COMMON_FLAGS = \
	-DUSE_SPIRV_CROSS_LIB \
	-std=c++17 \
	-s WASM=1 \
	-s MODULARIZE=1 \
	-s EXPORT_ES6=1 \
	-s EXPORTED_FUNCTIONS='["_compile", "_getVersion", "_getSymbols", "_malloc", "_free"]' \
	-s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "UTF8ToString", "stringToUTF8", "lengthBytesUTF8", "FS"]' \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s TOTAL_MEMORY=33554432 \
	-s STACK_SIZE=1048576 \
	-fno-exceptions

WASM_RELEASE_OPT = -O3 -DNDEBUG -flto
WASM_DEBUG_OPT = -O0 -g -s ASSERTIONS=2 -s SAFE_HEAP=1

# BWSL sources need defs_wasm.h for BSD type compatibility
WASM_BWSL_INCLUDES = -I. -Itools -include tools/defs_wasm.h
# SPIRV-Cross wrapper must NOT have defs_wasm.h (conflicts with f32/f64 member names)
WASM_SPIRV_INCLUDES = -I. -Itools

wasm: $(WASM_DIR) $(WASM_DIR)/bwsl_wasm.o $(WASM_DIR)/spirv_cross_wrapper.o
	$(EMCC) $(WASM_DIR)/bwsl_wasm.o $(WASM_DIR)/spirv_cross_wrapper.o \
		$(WASM_COMMON_FLAGS) $(WASM_RELEASE_OPT) -o $(WASM_DIR)/bwsl.js
	@echo "Built: $(WASM_DIR)/bwsl.js + $(WASM_DIR)/bwsl.wasm"
	@ls -lh $(WASM_DIR)/bwsl.*

$(WASM_DIR)/bwsl_wasm.o: $(WASM_SOURCE) | $(WASM_DIR)
	$(EMCC) -c $(WASM_SOURCE) $(WASM_BWSL_INCLUDES) -DBWSL_WASM \
		$(WASM_COMMON_FLAGS) $(WASM_RELEASE_OPT) -o $@

$(WASM_DIR)/spirv_cross_wrapper.o: $(SPIRV_CROSS_WRAPPER) | $(WASM_DIR)
	$(EMCC) -c $(SPIRV_CROSS_WRAPPER) $(WASM_SPIRV_INCLUDES) \
		-DSPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS \
		$(WASM_COMMON_FLAGS) $(WASM_RELEASE_OPT) -o $@

$(WASM_DIR):
	mkdir -p $(WASM_DIR)

wasm-debug: $(WASM_DIR)
	$(EMCC) -c $(WASM_SOURCE) $(WASM_BWSL_INCLUDES) -DBWSL_WASM \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/bwsl_wasm_debug.o
	$(EMCC) -c $(SPIRV_CROSS_WRAPPER) $(WASM_SPIRV_INCLUDES) \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/spirv_cross_wrapper_debug.o
	$(EMCC) $(WASM_DIR)/bwsl_wasm_debug.o $(WASM_DIR)/spirv_cross_wrapper_debug.o \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/bwsl-debug.js
	@echo "Built: $(WASM_DIR)/bwsl-debug.js"

# ============================================================================
# Testing (TODO: regression test battery)
# ============================================================================

test: bwslc
	@echo "Running regression tests..."
	@echo "(TODO: implement test battery)"

# ============================================================================
# Install WASM to bwsl-docs
# ============================================================================

DOCS_DIR = /Users/apresthus/Dev/bwsl-docs/public/wasm

install: wasm
	mkdir -p $(DOCS_DIR)
	cp $(WASM_DIR)/bwsl.js $(WASM_DIR)/bwsl.wasm $(DOCS_DIR)/
	@echo "Installed to $(DOCS_DIR)"
	@ls -lh $(DOCS_DIR)/bwsl.*

# ============================================================================
# Clean
# ============================================================================

clean:
	rm -rf $(BUILD_DIR)
