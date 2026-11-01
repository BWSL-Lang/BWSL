#pragma once
#include "core/bwsl_arena.h"
#include "core/bwsl_defs.h"
#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {

// Fragment-shader derivatives (ddx/ddy/fwidth and their variants) and
// implicit-LOD sampling (sample, sample_bias, sample_cmp and their offset
// forms) that run in control flow the pixels of a 2x2 quad may take
// differently. Their results are undefined in such quads on every target
// (GLSL, HLSL, MSL, SPIR-V and WGSL with its uniformity analysis off).
//
// A branch is divergent when its condition depends on a value that can
// differ within a quad: an interpolated input or the fragment position, or
// anything computed from one (texture reads at such coordinates, values
// chosen by a divergent branch, memory written from one). Uniforms,
// constants and flat inputs are the same for every pixel of a primitive,
// and a quad never spans primitives, so branches on them are safe.
// `discard` keeps the pixel as a helper for its quad's derivatives (WGSL's
// demote, and what drivers do), so it does not make what follows divergent.
// The analysis is conservative: memory is tracked per local variable and
// first-level struct field, not per array element.
struct QuadDivergenceData {
    u32* derivatives = nullptr;  // Derivative instructions in divergent control flow
    u32* branches = nullptr;     // For each, a divergent branch that controls it
    u32 count = 0;
};

namespace QuadDivergence {
    // Fills `result` from `ir` and its `cfg` (after SSA). Returns false when
    // the arena ran out; `result` is then empty.
    bool FindDivergentDerivatives(QuadDivergenceData* result, const IR::IRProgram* ir,
                                  const CFG* cfg, BWSL_Arena* arena);

    // True for the opcodes FindDivergentDerivatives looks for.
    bool IsImplicitDerivative(u16 opcode);

    // The BWSL intrinsic an implicit-derivative opcode came from ("fwidth").
    const char* IntrinsicName(u16 opcode);
}

} // namespace BWSL
