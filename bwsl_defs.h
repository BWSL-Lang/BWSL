// BWSL Type Definitions
// Standalone type aliases for BWSL compiler
// No external dependencies

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstddef>

// BSD-specific types that some systems don't have
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

// Core type aliases (using instead of macros to avoid conflicts with SIMDe)
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Result codes used by arena allocator
enum BWEResult {
    FAILURE = 0,
    SUCCESS = 1
};
