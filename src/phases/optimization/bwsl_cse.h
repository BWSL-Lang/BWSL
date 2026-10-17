#pragma once

#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {
namespace Optimization {

// Common subexpression elimination over the IR after constant propagation.
// A value instruction that computes what an instruction dominating it
// already computed (same operation, result type and operand values) becomes
// a copy of that result, which the backends alias. Copies carry their
// source's value, so a repeated chain collapses as a whole.
//
// With a CFG, blocks are walked down the dominator tree, and a value is
// available in the blocks its own dominates. `cfg` is null, or has one
// block, for straight-line stages: they're walked in order, and since a
// register there can be assigned more than once, a result is reused only
// while its register still holds it.
//
// Only operations whose result depends on nothing but their operands
// count: not loads, texture sampling or derivatives.
void EliminateCommonSubexpressions(IR::IRBuilder* builder, CFG* cfg);

}  // namespace Optimization
}  // namespace BWSL
