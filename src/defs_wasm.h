// WASM compatibility layer
// Provides BSD-specific types that musl libc doesn't have
// These must be defined before any BWSL headers are included

#pragma once

#include <cstdint>

// BSD-specific types that musl libc doesn't have
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

// Note: Type aliases (u8, u16, u32, f32, etc.) are now defined
// in bwsl_defs.h using 'using' declarations, not macros.
// This avoids conflicts with SIMDe and SPIRV-Cross.
