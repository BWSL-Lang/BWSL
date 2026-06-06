#pragma once
#include "bwsl_spirv_backend.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_utils.h"
#include "vendor/SPIRV-Headers/include/spirv/unified1/GLSL.std.450.h"
#include <array>
#include <cstring>

// =============================================================================
// SPIR-V backend implementation table of contents
// =============================================================================
// The backend keeps one SPIRVBuilder state object with the existing SoA-style
// arrays, caches, and section buffers. These slices are included into this
// translation unit in dependency order so the unity build and data layout remain
// unchanged.
//
// 1. bwsl_spirv_backend_tables.inl    - opcode lookup tables
// 2. bwsl_spirv_backend_core.inl      - initialization, preamble, entry points
// 3. bwsl_spirv_backend_types.inl     - type caches and sampled texture loading
// 4. bwsl_spirv_backend_constants.inl - constants, sections, decorations, names
// 5. bwsl_spirv_backend_operands.inl  - result/operand type helpers
// 6. bwsl_spirv_backend_ops.inl       - main IR opcode translation
// 7. bwsl_spirv_backend_control.inl   - CFG, structured control flow, phis
// 8. bwsl_spirv_backend_io.inl        - entry IO, builtins, shared vars, pulling
// 9. bwsl_spirv_backend_resources.inl - resource and descriptor declarations
// 10. bwsl_spirv_backend_finalize.inl - final assembly and growth helpers

#include "bwsl_spirv_backend_tables.inl"
#include "bwsl_spirv_backend_core.inl"
#include "bwsl_spirv_backend_types.inl"
#include "bwsl_spirv_backend_constants.inl"
#include "bwsl_spirv_backend_operands.inl"
#include "bwsl_spirv_backend_ops.inl"
#include "bwsl_spirv_backend_control.inl"
#include "bwsl_spirv_backend_io.inl"
#include "bwsl_spirv_backend_resources.inl"
#include "bwsl_spirv_backend_finalize.inl"
