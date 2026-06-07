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
BWSLC_SANITIZE_OUT = $(BUILD_DIR)/bwslc-sanitize$(EXE_EXT)
BWSLC_WIN_ZIG_OUT = $(BUILD_DIR)/bwslc-win.exe
BWSLC_WIN_ZIG_DEBUG_OUT = $(BUILD_DIR)/bwslc-win-debug.exe

EQUIV_RUNNER_SRC = tools/equiv_runner.cpp
EQUIV_RUNNER_OUT = $(BUILD_DIR)/equiv_runner$(EXE_EXT)

# Vulkan SDK location (override by exporting VULKAN_SDK)
ifeq ($(HOST_OS),macos)
VULKAN_SDK ?= $(HOME)/VulkanSDK/1.3.243.0/macOS
VULKAN_INCLUDE = -I$(VULKAN_SDK)/include
VULKAN_LIBS = -L$(VULKAN_SDK)/lib -lvulkan -Wl,-rpath,$(VULKAN_SDK)/lib
else ifeq ($(HOST_OS),linux)
VULKAN_INCLUDE = $(if $(VULKAN_SDK),-I$(VULKAN_SDK)/include,)
VULKAN_LIBS = $(if $(VULKAN_SDK),-L$(VULKAN_SDK)/lib,) -lvulkan
else
VULKAN_INCLUDE =
VULKAN_LIBS =
endif

# SPIRV-Cross integration for cross-compilation
SPIRV_CROSS_DIR = vendor/SPIRV-Cross
SPIRV_CROSS_INCLUDES = -I$(SPIRV_CROSS_DIR)
SPIRV_CROSS_FLAGS = -DUSE_SPIRV_CROSS_LIB

# SPIRV-Tools. Native builds link the vendored static library for in-process
# validation; the CLI tools remain available for tests/fallbacks.
SPIRV_TOOLS_SRC   = vendor/SPIRV-Tools
SPIRV_HEADERS_SRC = vendor/SPIRV-Headers
SPIRV_TOOLS_BUILD = $(BUILD_DIR)/spirv-tools-build
SPIRV_TOOLS_STAMP = $(SPIRV_TOOLS_BUILD)/.bwsl-built
SPIRV_TOOLS_INCLUDES = -I$(SPIRV_TOOLS_SRC)/include -I$(SPIRV_HEADERS_SRC)/include
ifeq ($(HOST_OS),windows)
SPIRV_TOOLS_LIB = $(SPIRV_TOOLS_BUILD)/source/Release/SPIRV-Tools.lib
SPIRV_TOOLS_BUILD_CONFIG = --config Release
MSVC_SPIRV_TOOLS_INCLUDES = /Ivendor\SPIRV-Tools\include /Ivendor\SPIRV-Headers\include
else
SPIRV_TOOLS_LIB = $(SPIRV_TOOLS_BUILD)/source/libSPIRV-Tools.a
SPIRV_TOOLS_BUILD_CONFIG =
endif

USE_LINKED_SPIRV_TOOLS ?= 1
NATIVE_SPIRV_TOOLS_PREREQS =
NATIVE_SPIRV_TOOLS_FLAGS =
NATIVE_SPIRV_TOOLS_INCLUDES =
NATIVE_SPIRV_TOOLS_LIBS =
ifneq ($(USE_LINKED_SPIRV_TOOLS),0)
NATIVE_SPIRV_TOOLS_PREREQS = $(SPIRV_TOOLS_STAMP)
ifeq ($(HOST_OS),windows)
NATIVE_SPIRV_TOOLS_FLAGS = /DUSE_SPIRV_TOOLS_LIB
NATIVE_SPIRV_TOOLS_INCLUDES = $(MSVC_SPIRV_TOOLS_INCLUDES)
NATIVE_SPIRV_TOOLS_LIBS = $(subst /,\,$(SPIRV_TOOLS_LIB))
else
NATIVE_SPIRV_TOOLS_FLAGS = -DUSE_SPIRV_TOOLS_LIB
NATIVE_SPIRV_TOOLS_INCLUDES = $(SPIRV_TOOLS_INCLUDES)
NATIVE_SPIRV_TOOLS_LIBS = $(SPIRV_TOOLS_LIB)
endif
endif

CCACHE := $(shell command -v ccache 2>/dev/null)
SPIRV_CMAKE_LAUNCHER = $(if $(CCACHE),\
	-DCMAKE_C_COMPILER_LAUNCHER=ccache \
	-DCMAKE_CXX_COMPILER_LAUNCHER=ccache,)

SPIRV_TEST_DEPS =
ifneq ($(HOST_OS),windows)
HAS_SPIRV_VAL := $(shell command -v spirv-val 2>/dev/null)
HAS_SPIRV_DIS := $(shell command -v spirv-dis 2>/dev/null)
ifeq ($(and $(HAS_SPIRV_VAL),$(HAS_SPIRV_DIS)),)
SPIRV_TEST_DEPS = spirv-tools
endif
endif

BWSL_INCLUDE_DIRS = -I. -Icore -Icore/middleware \
	-Iphases/lexing -Iphases/parser -Iphases/evaluation \
	-Iphases/ir_generation -Iphases/ir_lowering -Iphases/control_flow \
	-Iphases/ssa -Iphases/backends/spirv -Iphases/backends/gles
MSVC_BWSL_INCLUDE_DIRS = /I. /Icore /Icore/middleware \
	/Iphases/lexing /Iphases/parser /Iphases/evaluation \
	/Iphases/ir_generation /Iphases/ir_lowering /Iphases/control_flow \
	/Iphases/ssa /Iphases/backends/spirv /Iphases/backends/gles

# Native Unix toolchain
ifeq ($(origin CXX), default)
CXX = clang++
endif
ifneq ($(CCACHE),)
ifeq ($(findstring ccache,$(firstword $(CXX))),)
CXX := $(CCACHE) $(CXX)
endif
endif
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra
CXXFLAGS_DEBUG ?= -g -O0 -std=c++20 -Wall -Wextra

# Native Windows MSVC toolchain
MSVC_CXX ?= cl
MSVC_COMMON_FLAGS = /nologo /std:c++20 /EHsc /DUSE_SPIRV_CROSS_LIB /I$(SPIRV_CROSS_DIR) $(MSVC_BWSL_INCLUDE_DIRS)
MSVC_RELEASE_FLAGS ?= /O2 /W4
MSVC_DEBUG_FLAGS ?= /Zi /Od /W4
MSVC_LINK_FLAGS ?= /link /STACK:8388608

# Zig Windows cross-compilation
ZIG ?= zig
ZIG_WIN_TARGET ?= x86_64-windows-gnu
ZIG_RELEASE_FLAGS ?= -OReleaseFast -std=c++20 -Wall -Wextra
ZIG_DEBUG_FLAGS ?= -ODebug -g -std=c++20 -Wall -Wextra

WIN_BUILD_DIR = $(subst /,\,$(BUILD_DIR))
WIN_WASM_DIR = $(subst /,\,$(WASM_DIR))
WIN_BWSLC_OUT = $(subst /,\,$(BWSLC_OUT))
WIN_BWSLC_DEBUG_OUT = $(subst /,\,$(BWSLC_DEBUG_OUT))

# ============================================================================
# Platform-specific commands and object file rules
# ============================================================================

ifeq ($(HOST_OS),windows)
SPIRV_CROSS_WRAPPER_OBJ = $(BUILD_DIR)/spirv_cross_wrapper.obj
SPIRV_CROSS_WRAPPER_DEBUG_OBJ = $(BUILD_DIR)/spirv_cross_wrapper_debug.obj
BWSLC_OBJ = $(BUILD_DIR)/bwslc.obj
BWSLC_DEBUG_OBJ = $(BUILD_DIR)/bwslc_debug.obj
BWSLC_PREREQS = $(SPIRV_CROSS_WRAPPER_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
BWSLC_DEBUG_PREREQS = $(SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
BWSLC_COMPILE_CMD = $(MSVC_CXX) $(MSVC_RELEASE_FLAGS) $(MSVC_COMMON_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) $(NATIVE_SPIRV_TOOLS_INCLUDES) /c /Fo$(BWSLC_OBJ) $(BWSLC_SRC)
BWSLC_DEBUG_COMPILE_CMD = $(MSVC_CXX) $(MSVC_DEBUG_FLAGS) $(MSVC_COMMON_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) $(NATIVE_SPIRV_TOOLS_INCLUDES) /Fd$(WIN_BUILD_DIR)\bwslc-debug.pdb /c /Fo$(BWSLC_DEBUG_OBJ) $(BWSLC_SRC)
NATIVE_BWSLC_CMD = $(MSVC_CXX) $(MSVC_RELEASE_FLAGS) $(MSVC_COMMON_FLAGS) \
	/Fe$(WIN_BWSLC_OUT) $(SPIRV_CROSS_WRAPPER_OBJ) $(BWSLC_OBJ) $(NATIVE_SPIRV_TOOLS_LIBS) $(MSVC_LINK_FLAGS)
NATIVE_BWSLC_DEBUG_CMD = $(MSVC_CXX) $(MSVC_DEBUG_FLAGS) $(MSVC_COMMON_FLAGS) \
	/Fe$(WIN_BWSLC_DEBUG_OUT) $(SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(BWSLC_DEBUG_OBJ) $(NATIVE_SPIRV_TOOLS_LIBS) $(MSVC_LINK_FLAGS)
MKDIR_BUILD = cmd /C if not exist "$(WIN_BUILD_DIR)" mkdir "$(WIN_BUILD_DIR)"
MKDIR_WASM = cmd /C if not exist "$(WIN_WASM_DIR)" mkdir "$(WIN_WASM_DIR)"
CLEAN_BUILD = cmd /C if exist "$(WIN_BUILD_DIR)" rmdir /S /Q "$(WIN_BUILD_DIR)"

$(SPIRV_CROSS_WRAPPER_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(MSVC_CXX) $(MSVC_RELEASE_FLAGS) $(MSVC_COMMON_FLAGS) /c /Fo$@ $<

$(SPIRV_CROSS_WRAPPER_DEBUG_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(MSVC_CXX) $(MSVC_DEBUG_FLAGS) $(MSVC_COMMON_FLAGS) /Fd$(WIN_BUILD_DIR)\bwslc-debug.pdb /c /Fo$@ $<

$(BWSLC_OBJ): $(BWSLC_SRC) $(NATIVE_SPIRV_TOOLS_PREREQS) | $(BUILD_DIR)
	$(MSVC_CXX) $(MSVC_RELEASE_FLAGS) $(MSVC_COMMON_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) $(NATIVE_SPIRV_TOOLS_INCLUDES) /c /Fo$@ $<

$(BWSLC_DEBUG_OBJ): $(BWSLC_SRC) $(NATIVE_SPIRV_TOOLS_PREREQS) | $(BUILD_DIR)
	$(MSVC_CXX) $(MSVC_DEBUG_FLAGS) $(MSVC_COMMON_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) $(NATIVE_SPIRV_TOOLS_INCLUDES) /Fd$(WIN_BUILD_DIR)\bwslc-debug.pdb /c /Fo$@ $<
else
SPIRV_CROSS_WRAPPER_OBJ = $(BUILD_DIR)/spirv_cross_wrapper.o
SPIRV_CROSS_WRAPPER_DEBUG_OBJ = $(BUILD_DIR)/spirv_cross_wrapper_debug.o
BWSLC_OBJ = $(BUILD_DIR)/bwslc.o
BWSLC_DEBUG_OBJ = $(BUILD_DIR)/bwslc_debug.o
BWSLC_PREREQS = $(SPIRV_CROSS_WRAPPER_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
BWSLC_DEBUG_PREREQS = $(SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
BWSLC_COMPILE_CMD = $(CXX) -c $(CXXFLAGS) $(SPIRV_CROSS_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) \
	$(SPIRV_CROSS_INCLUDES) $(NATIVE_SPIRV_TOOLS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
	-o $(BWSLC_OBJ) $(BWSLC_SRC)
BWSLC_DEBUG_COMPILE_CMD = $(CXX) -c $(CXXFLAGS_DEBUG) $(SPIRV_CROSS_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) \
	$(SPIRV_CROSS_INCLUDES) $(NATIVE_SPIRV_TOOLS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
	-o $(BWSLC_DEBUG_OBJ) $(BWSLC_SRC)
NATIVE_BWSLC_CMD = $(CXX) $(CXXFLAGS) -o $(BWSLC_OUT) $(SPIRV_CROSS_WRAPPER_OBJ) $(BWSLC_OBJ) $(NATIVE_SPIRV_TOOLS_LIBS)
NATIVE_BWSLC_DEBUG_CMD = $(CXX) $(CXXFLAGS_DEBUG) -o $(BWSLC_DEBUG_OUT) $(SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(BWSLC_DEBUG_OBJ) $(NATIVE_SPIRV_TOOLS_LIBS)
MKDIR_BUILD = mkdir -p $(BUILD_DIR)
MKDIR_WASM = mkdir -p $(WASM_DIR)
CLEAN_BUILD = rm -rf $(BUILD_DIR)

$(SPIRV_CROSS_WRAPPER_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
		-o $@ $<

$(SPIRV_CROSS_WRAPPER_DEBUG_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS_DEBUG) $(SPIRV_CROSS_FLAGS) $(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
		-o $@ $<

$(BWSLC_OBJ): $(BWSLC_SRC) $(NATIVE_SPIRV_TOOLS_PREREQS) | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $(SPIRV_CROSS_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(NATIVE_SPIRV_TOOLS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
		-o $@ $<

$(BWSLC_DEBUG_OBJ): $(BWSLC_SRC) $(NATIVE_SPIRV_TOOLS_PREREQS) | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS_DEBUG) $(SPIRV_CROSS_FLAGS) $(NATIVE_SPIRV_TOOLS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(NATIVE_SPIRV_TOOLS_INCLUDES) $(BWSL_INCLUDE_DIRS) \
		-o $@ $<
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
	-s STACK_SIZE=1048576

WASM_RELEASE_OPT = -O3 -DNDEBUG -flto
WASM_DEBUG_OPT = -O0 -g -s ASSERTIONS=2 -s SAFE_HEAP=1

# BWSL sources need defs_wasm.h for BSD type compatibility
WASM_BWSL_INCLUDES = $(BWSL_INCLUDE_DIRS) -Itools -include tools/defs_wasm.h
# SPIRV-Cross wrapper must NOT have defs_wasm.h (conflicts with f32/f64 member names)
WASM_SPIRV_INCLUDES = $(BWSL_INCLUDE_DIRS) -Itools

# ============================================================================
# Targets
# ============================================================================

.PHONY: all help bwslc bwslc-debug bwslc-sanitize bwslc-msvc bwslc-msvc-debug \
	bwslc-win-zig bwslc-win-zig-debug clean wasm wasm-debug test test-sanitize \
	install equiv_runner spirv-tools clangd-config

all: bwslc

help:
	@echo "BWSL build targets:"
	@echo "  make bwslc              Native CLI compiler"
	@echo "  make bwslc-debug        Native CLI compiler with debug info"
	@echo "  make bwslc-msvc         Native Windows build with MSVC"
	@echo "  make bwslc-win-zig      Windows cross-build with Zig"
	@echo "  make wasm               WebAssembly module"
	@echo "  make wasm-debug         WebAssembly module with debug info"
	@echo "  make spirv-tools        Build SPIRV-Tools library and CLI tools"
	@echo "  make install DOCS_DIR=/path/to/site/public/wasm"
	@echo "  make clean              Remove build artifacts"

bwslc: $(BWSLC_PREREQS)
	$(BWSLC_COMPILE_CMD)
	$(NATIVE_BWSLC_CMD)
	@echo "Built: $(BWSLC_OUT)"

bwslc-debug: $(BWSLC_DEBUG_PREREQS)
	$(BWSLC_DEBUG_COMPILE_CMD)
	$(NATIVE_BWSLC_DEBUG_CMD)
	@echo "Built: $(BWSLC_DEBUG_OUT)"

$(BWSLC_OUT): $(SPIRV_CROSS_WRAPPER_OBJ) $(BWSLC_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
	$(NATIVE_BWSLC_CMD)

$(BWSLC_DEBUG_OUT): $(SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(BWSLC_DEBUG_OBJ) $(NATIVE_SPIRV_TOOLS_PREREQS)
	$(NATIVE_BWSLC_DEBUG_CMD)

# Sanitizer build: ASan + UBSan, no optimization, frame pointers retained.
# Used by the regression harness (`make test-sanitize`) to catch memory and
# undefined-behavior bugs that the release build silently tolerates.
SANITIZE_CXX ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang++ ]; then \
	echo /opt/homebrew/opt/llvm/bin/clang++; \
	elif [ -x /usr/local/opt/llvm/bin/clang++ ]; then \
	echo /usr/local/opt/llvm/bin/clang++; \
	else echo $(CXX); fi)
SANITIZE_FLAGS = -std=c++20 -Wall -Wextra -g -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=all
SANITIZE_LINK_FLAGS =
ifeq ($(SANITIZE_CXX),/opt/homebrew/opt/llvm/bin/clang++)
SANITIZE_LINK_FLAGS = -stdlib=libc++ -L/opt/homebrew/opt/llvm/lib/c++ \
	-Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++
endif
ifeq ($(SANITIZE_CXX),/usr/local/opt/llvm/bin/clang++)
SANITIZE_LINK_FLAGS = -stdlib=libc++ -L/usr/local/opt/llvm/lib/c++ \
	-Wl,-rpath,/usr/local/opt/llvm/lib/c++
endif

bwslc-sanitize: $(BUILD_DIR) $(NATIVE_SPIRV_TOOLS_PREREQS)
	$(SANITIZE_CXX) $(SANITIZE_FLAGS) $(SANITIZE_LINK_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(NATIVE_SPIRV_TOOLS_FLAGS) $(SPIRV_CROSS_INCLUDES) $(NATIVE_SPIRV_TOOLS_INCLUDES) \
		$(BWSL_INCLUDE_DIRS) -o $(BWSLC_SANITIZE_OUT) $(SPIRV_CROSS_WRAPPER) $(BWSLC_SRC) \
		$(NATIVE_SPIRV_TOOLS_LIBS)
	@echo "Built: $(BWSLC_SANITIZE_OUT)"

# Run the Python regression harness against the sanitized binary. We pass
# --compiler so run_tests.py uses the sanitized build instead of build/bwslc,
# and halt_on_error + abort_on_error make any ASan/UBSan hit a test failure.
test-sanitize: bwslc-sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
	python3 tests/run_tests.py --compiler $(BWSLC_SANITIZE_OUT) --no-spirv-val

equiv_runner: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(VULKAN_INCLUDE) -o $(EQUIV_RUNNER_OUT) \
		$(EQUIV_RUNNER_SRC) $(VULKAN_LIBS)
	@echo "Built: $(EQUIV_RUNNER_OUT)"

spirv-tools: $(SPIRV_TOOLS_STAMP)

$(SPIRV_TOOLS_STAMP): $(SPIRV_TOOLS_SRC)/CMakeLists.txt | $(BUILD_DIR)
	cmake -S $(SPIRV_TOOLS_SRC) \
		-B $(SPIRV_TOOLS_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		"-DSPIRV-Headers_SOURCE_DIR=$(abspath $(SPIRV_HEADERS_SRC))" \
		$(SPIRV_CMAKE_LAUNCHER) \
		-DSPIRV_SKIP_TESTS=ON \
		-DSPIRV_WERROR=OFF \
		-DSPIRV_BUILD_FUZZER=OFF
	cmake --build $(SPIRV_TOOLS_BUILD) $(SPIRV_TOOLS_BUILD_CONFIG) --target SPIRV-Tools-static spirv-val spirv-dis --parallel
	cmake -E touch $(SPIRV_TOOLS_STAMP)
	@echo "Built: $(SPIRV_TOOLS_LIB) and $(SPIRV_TOOLS_BUILD)/tools/spirv-val spirv-dis"

# libFuzzer build. Apple's bundled clang ships without libclang_rt.fuzzer on
# some Xcode versions, so default to Homebrew LLVM when it's installed.
# Override with e.g. `make bwslc-fuzz FUZZ_CXX=/path/to/clang++`.
FUZZ_SRC = tools/bwslc_fuzz.cpp
FUZZ_OUT = $(BUILD_DIR)/bwslc-fuzz$(EXE_EXT)
FUZZ_CXX ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang++ ]; then \
	echo /opt/homebrew/opt/llvm/bin/clang++; \
	elif [ -x /usr/local/opt/llvm/bin/clang++ ]; then \
	echo /usr/local/opt/llvm/bin/clang++; \
	else echo clang++; fi)
FUZZ_FLAGS = -fsanitize=fuzzer,address,undefined -g -O1 -std=c++20 \
	-fno-omit-frame-pointer -Wall
# Homebrew LLVM's fuzzer runtime links against its own libc++; match that
# with stdlib + rpath so we don't collide with the system libc++.
FUZZ_LINK_FLAGS =
ifeq ($(FUZZ_CXX),/opt/homebrew/opt/llvm/bin/clang++)
FUZZ_LINK_FLAGS = -stdlib=libc++ -L/opt/homebrew/opt/llvm/lib/c++ \
	-Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++
endif
ifeq ($(FUZZ_CXX),/usr/local/opt/llvm/bin/clang++)
FUZZ_LINK_FLAGS = -stdlib=libc++ -L/usr/local/opt/llvm/lib/c++ \
	-Wl,-rpath,/usr/local/opt/llvm/lib/c++
endif

bwslc-fuzz: $(BUILD_DIR)
	$(FUZZ_CXX) $(FUZZ_FLAGS) $(FUZZ_LINK_FLAGS) $(BWSL_INCLUDE_DIRS) -o $(FUZZ_OUT) $(FUZZ_SRC)
	@echo "Built: $(FUZZ_OUT)"

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

ZIG_SPIRV_CROSS_WRAPPER_OBJ = $(BUILD_DIR)/spirv_cross_wrapper_win.o
ZIG_SPIRV_CROSS_WRAPPER_DEBUG_OBJ = $(BUILD_DIR)/spirv_cross_wrapper_win_debug.o
ZIG_BWSLC_OBJ = $(BUILD_DIR)/bwslc_win.o
ZIG_BWSLC_DEBUG_OBJ = $(BUILD_DIR)/bwslc_win_debug.o

$(ZIG_SPIRV_CROSS_WRAPPER_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(ZIG) c++ -c -target $(ZIG_WIN_TARGET) $(ZIG_RELEASE_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) -o $@ $<

$(ZIG_SPIRV_CROSS_WRAPPER_DEBUG_OBJ): $(SPIRV_CROSS_WRAPPER) | $(BUILD_DIR)
	$(ZIG) c++ -c -target $(ZIG_WIN_TARGET) $(ZIG_DEBUG_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) -o $@ $<

$(ZIG_BWSLC_OBJ): $(BWSLC_SRC) | $(BUILD_DIR)
	$(ZIG) c++ -c -target $(ZIG_WIN_TARGET) $(ZIG_RELEASE_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) -o $@ $<

$(ZIG_BWSLC_DEBUG_OBJ): $(BWSLC_SRC) | $(BUILD_DIR)
	$(ZIG) c++ -c -target $(ZIG_WIN_TARGET) $(ZIG_DEBUG_FLAGS) $(SPIRV_CROSS_FLAGS) \
		$(SPIRV_CROSS_INCLUDES) $(BWSL_INCLUDE_DIRS) -o $@ $<

bwslc-win-zig: $(ZIG_SPIRV_CROSS_WRAPPER_OBJ) $(ZIG_BWSLC_OBJ)
	$(ZIG) c++ -target $(ZIG_WIN_TARGET) $(ZIG_RELEASE_FLAGS) \
		-o $(BWSLC_WIN_ZIG_OUT) $(ZIG_SPIRV_CROSS_WRAPPER_OBJ) $(ZIG_BWSLC_OBJ)
	@echo "Built: $(BWSLC_WIN_ZIG_OUT)"

bwslc-win-zig-debug: $(ZIG_SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(ZIG_BWSLC_DEBUG_OBJ)
	$(ZIG) c++ -target $(ZIG_WIN_TARGET) $(ZIG_DEBUG_FLAGS) \
		-o $(BWSLC_WIN_ZIG_DEBUG_OUT) $(ZIG_SPIRV_CROSS_WRAPPER_DEBUG_OBJ) $(ZIG_BWSLC_DEBUG_OBJ)
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
test: bwslc $(SPIRV_TEST_DEPS)
	PATH="$(abspath $(SPIRV_TOOLS_BUILD)/tools):$$PATH" ./tests/run_tests.sh
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

# ============================================================================
# IDE Configuration
# ============================================================================

clangd-config:
	python3 scripts/gen_compile_commands.py
