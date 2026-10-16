#pragma once

#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {
namespace Optimization {

// Sparse conditional constant propagation (Wegman & Zadeck) over the IR
// after SSA. It folds instructions whose operands are constant, scalars,
// vectors and matrices alike, intrinsics included, resolves phis from the
// edges that can actually run, and makes branch conditions constant where
// they are. A loop whose inputs are all constant and that has no side
// effects is run at compile time, so code after it sees its results. Uses
// of a constant scalar register are replaced by the constant, a constant
// vector is built from constants, and the value instructions and phis
// nothing reads any more are removed.
//
// The CFG and block layout are left alone: a branch or switch with a
// constant condition stays one, and the backends emit only the edge it takes.
// `cfg` is null for straight-line stages, which skip CFG and SSA. Those,
// and single-block stages, get one forward walk instead, since a register
// there can be assigned more than once.
void PropagateConstants(IR::IRBuilder* builder, CFG* cfg);

}  // namespace Optimization
}  // namespace BWSL
