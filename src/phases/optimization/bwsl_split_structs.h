#pragma once

#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {
namespace Optimization {

// Splits local struct variables into one register per field, after lowering
// and before CFG construction and SSA. A field write through a field
// pointer becomes an assignment of the field's register and a field read a
// copy of it, so fields get phis, constants, ranges and CSE like any other
// variable, and the struct's memory and its copies disappear. Where the
// whole struct is read (copied into a struct that isn't split, stored to a
// buffer, selected), it's rebuilt from the fields there. A struct field
// that is itself a struct is split in a following round.
//
// A struct isn't split when its address is taken as a whole, one of its
// fields is an array, a field pointer escapes (is copied, passed or
// indexed), or an instruction that may read a struct reads it in a way
// this pass doesn't rewrite.
void SplitLocalStructs(IR::IRBuilder* builder);

}  // namespace Optimization
}  // namespace BWSL
