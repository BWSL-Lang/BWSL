# BWSL Compiler Build System
# ============================
#
# Targets:
#   make bwslc              - Build the native CLI compiler for the host
#   make bwslc-debug        - Build the native CLI compiler with debug symbols
#   make bwslc-msvc         - Build the native CLI compiler with MSVC on Windows
#   make bwslc-win-zig      - Cross-compile a Windows CLI compiler with Zig
#   make wasm               - Build WebAssembly module
#   make wasm-debug         - Build WebAssembly module with debug info
#   make clean              - Remove all build artifacts
#   make test               - Run regression tests
#   make install            - Copy WASM artifacts to DOCS_DIR
#
# Native toolchains:
#   - macOS/Linux: uses CXX (defaults to clang++)
#   - Windows: uses cl.exe from a Visual Studio Developer Command Prompt
#
# Cross-compilation:
#   - macOS/Linux -> Windows: make bwslc-win-zig

ifeq ($(OS),Windows_NT)
HOST_OS = windows
EXE_EXT = .exe
else
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
HOST_OS = macos
else ifeq ($(UNAME_S),Linux)
HOST_OS = linux
else
HOST_OS = unknown
endif
EXE_EXT =
endif

# Source paths
BWSLC_SRC = tools/bwslc.cpp
SPIRV_CROSS_WRAPPER = tools/spirv_cross_wrapper.cpp
WASM_SOURCE = tools/bwsl_wasm.cpp

# Output directory
BUILD_DIR = build
WASM_DIR = $(BUILD_DIR)/wasm

BWSLC_OUT = $(BUILD_DIR)/bwslc$(EXE_EXT)
BWSLC_DEBUG_OUT = $(BUILD_DIR)/bwslc-debug$(EXE_EXT)
BWSLC_WIN_ZIG_OUT = $(BUILD_DIR)/bwslc-win.exe
BWSLC_WIN_ZIG_DEBUG_OUT = $(BUILD_DIR)/bwslc-win-debug.exe

# SPIRV-Cross integration for cross-compilation
SPIRV_CROSS_DIR = vendor/SPIRV-Cross
SPIRV_CROSS_INCLUDES = -I$(SPIRV_CROSS_DIR)
SPIRV_CROSS_FLAGS = -DUSE_SPIRV_CROSS_LIB

# Native Unix toolchain
ifeq ($(origin CXX), default)
CXX = clang++
endif
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra
CXXFLAGS_DEBUG ?= -g -O0 -std=c++20 -Wall -Wextra

# Native Windows MSVC toolchain
MSVC_CXX ?= cl
MSVC_COMMON_FLAGS = /nologo /std:c++20 /EHsc /DUSE_SPIRV_CROSS_LIB /I$(SPIRV_CROSS_DIR) /I.
MSVC_RELEASE_FLAGS ?= /O2 /W4
MSVC_DEBUG_FLAGS ?= /Zi /Od /W4

# Zig Windows cross-compilation
ZIG ?= zig
ZIG_WIN_TARGET ?= x86_64-windows-gnu
ZIG_RELEASE_FLAGS ?= -OReleaseFast -std=c++20 -Wall -Wextra
ZIG_DEBUG_FLAGS ?= -ODebug -g -std=c++20 -Wall -Wextra

WIN_BUILD_DIR = $(subst /,\,$(BUILD_DIR))
WIN_WASM_DIR = $(subst /,\,$(WASM_DIR))
WIN_BWSLC_OUT = $(subst /,\,$(BWSLC_OUT))
WIN_BWSLC_DEBUG_OUT = $(subst /,\,$(BWSLC_DEBUG_OUT))

ifeq ($(HOST_OS),windows)
NATIVE_BWSLC_CMD = $(MSVC_CXX) $(MSVC_RELEASE_FLAGS) $(MSVC_COMMON_FLAGS) \
	/Fo$(WIN_BUILD_DIR)\\ /Fe$(WIN_BWSLC_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
NATIVE_BWSLC_DEBUG_CMD = $(MSVC_CXX) $(MSVC_DEBUG_FLAGS) $(MSVC_COMMON_FLAGS) \
	/Fo$(WIN_BUILD_DIR)\\ /Fd$(WIN_BUILD_DIR)\bwslc-debug.pdb /Fe$(WIN_BWSLC_DEBUG_OUT) \
	$(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
MKDIR_BUILD = cmd /C if not exist "$(WIN_BUILD_DIR)" mkdir "$(WIN_BUILD_DIR)"
MKDIR_WASM = cmd /C if not exist "$(WIN_WASM_DIR)" mkdir "$(WIN_WASM_DIR)"
CLEAN_BUILD = cmd /C if exist "$(WIN_BUILD_DIR)" rmdir /S /Q "$(WIN_BUILD_DIR)"
else
NATIVE_BWSLC_CMD = $(CXX) $(CXXFLAGS) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) -I. \
	-o $(BWSLC_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
NATIVE_BWSLC_DEBUG_CMD = $(CXX) $(CXXFLAGS_DEBUG) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) -I. \
	-o $(BWSLC_DEBUG_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
MKDIR_BUILD = mkdir -p $(BUILD_DIR)
MKDIR_WASM = mkdir -p $(WASM_DIR)
CLEAN_BUILD = rm -rf $(BUILD_DIR)
endif

# ============================================================================
# WebAssembly Build
# ============================================================================

# Emscripten flags
EMCC = emcc
WASM_COMMON_FLAGS = \
	-DUSE_SPIRV_CROSS_LIB \
	-std=c++20 \
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

# ============================================================================
# Targets
# ============================================================================

.PHONY: all help bwslc bwslc-debug bwslc-msvc bwslc-msvc-debug \
	bwslc-win-zig bwslc-win-zig-debug clean wasm wasm-debug test install

all: bwslc

help:
	@echo "BWSL build targets:"
	@echo "  make bwslc              Native CLI compiler"
	@echo "  make bwslc-debug        Native CLI compiler with debug info"
	@echo "  make bwslc-msvc         Native Windows build with MSVC"
	@echo "  make bwslc-win-zig      Windows cross-build with Zig"
	@echo "  make wasm               WebAssembly module"
	@echo "  make wasm-debug         WebAssembly module with debug info"
	@echo "  make install DOCS_DIR=/path/to/site/public/wasm"
	@echo "  make clean              Remove build artifacts"

bwslc: $(BUILD_DIR)
	$(NATIVE_BWSLC_CMD)
	@echo "Built: $(BWSLC_OUT)"

bwslc-debug: $(BUILD_DIR)
	$(NATIVE_BWSLC_DEBUG_CMD)
	@echo "Built: $(BWSLC_DEBUG_OUT)"

ifeq ($(HOST_OS),windows)
bwslc-msvc: bwslc

bwslc-msvc-debug: bwslc-debug
else
bwslc-msvc:
	@echo "bwslc-msvc requires Windows with cl.exe available in a Developer Command Prompt." >&2
	@exit 1

bwslc-msvc-debug:
	@echo "bwslc-msvc-debug requires Windows with cl.exe available in a Developer Command Prompt." >&2
	@exit 1
endif

bwslc-win-zig: $(BUILD_DIR)
	$(ZIG) c++ -target $(ZIG_WIN_TARGET) $(ZIG_RELEASE_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) -I. -o $(BWSLC_WIN_ZIG_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
	@echo "Built: $(BWSLC_WIN_ZIG_OUT)"

bwslc-win-zig-debug: $(BUILD_DIR)
	$(ZIG) c++ -target $(ZIG_WIN_TARGET) $(ZIG_DEBUG_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) -I. -o $(BWSLC_WIN_ZIG_DEBUG_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC)
	@echo "Built: $(BWSLC_WIN_ZIG_DEBUG_OUT)"

$(BUILD_DIR):
	$(MKDIR_BUILD)

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

$(WASM_DIR): | $(BUILD_DIR)
	$(MKDIR_WASM)

wasm-debug: $(WASM_DIR)
	$(EMCC) -c $(WASM_SOURCE) $(WASM_BWSL_INCLUDES) -DBWSL_WASM \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/bwsl_wasm_debug.o
	$(EMCC) -c $(SPIRV_CROSS_WRAPPER) $(WASM_SPIRV_INCLUDES) \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/spirv_cross_wrapper_debug.o
	$(EMCC) $(WASM_DIR)/bwsl_wasm_debug.o $(WASM_DIR)/spirv_cross_wrapper_debug.o \
		$(WASM_COMMON_FLAGS) $(WASM_DEBUG_OPT) -o $(WASM_DIR)/bwsl-debug.js
	@echo "Built: $(WASM_DIR)/bwsl-debug.js"

ifeq ($(HOST_OS),windows)
test:
	@if where python >nul 2>&1; then \
		python tests/run_tests.py; \
	else \
		echo "Regression tests require Python on Windows." >&2; \
		exit 1; \
	fi
else
test: bwslc
	./tests/run_tests.sh
endif

# ============================================================================
# Install WASM to bwsl-docs
# ============================================================================

DOCS_DIR ?=

install: wasm
	@test -n "$(DOCS_DIR)" || (echo "Set DOCS_DIR=/path/to/site/public/wasm" >&2; exit 1)
	mkdir -p "$(DOCS_DIR)"
	cp $(WASM_DIR)/bwsl.js $(WASM_DIR)/bwsl.wasm "$(DOCS_DIR)/"
	@echo "Installed to $(DOCS_DIR)"
	@ls -lh "$(DOCS_DIR)"/bwsl.*

# ============================================================================
# Clean
# ============================================================================

clean:
	$(CLEAN_BUILD)
