#pragma once

#include "bwsl_defs.h"

namespace BWSL {

// The range a declaration promises for its value, from `@bounds(...)`:
// per component of a scalar or vector. A component with either bound is a
// number, never NaN; one with both is finite.
struct ComponentBounds {
    f64 low;
    f64 high;
    u8 flags;

    static constexpr u8 HAS_LOW = 1;
    static constexpr u8 HAS_HIGH = 2;
    static constexpr u8 LOW_EXCLUSIVE = 4;   // value > low rather than >= low
    static constexpr u8 HIGH_EXCLUSIVE = 8;  // value < high rather than <= high
};

struct DeclaredBounds {
    ComponentBounds components[4];
    u8 count;  // Components of the declared type: 1 for a scalar
};

}  // namespace BWSL
