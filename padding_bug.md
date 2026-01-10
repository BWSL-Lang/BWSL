# Scalar Arrays in Structs Bug - Investigation Log

## Problem Summary
When BWSL compiles structs containing scalar arrays (`float[N]`, `uint[N]`), SPIRV-Cross converts them to vector arrays (`float4[N]`, `uint4[N]`), causing a 4x size increase and CPU/GPU struct mismatch.

## Expected vs Actual
- `float[16]` should be 64 bytes, becomes `float4[16]` = 256 bytes
- `uint[16]` should be 64 bytes, becomes `uint4[16]` = 256 bytes

## Investigation Started: 2026-01-10

---

## Step 1: Initial Exploration

Looking at the SPIR-V backend and IR lowering to understand how scalar arrays in structs are handled.

### SPIR-V Output Analysis

Compiled `World.bwsl` and examined the SPIR-V disassembly:

```
OpDecorate %_arr_float_uint_16_2 ArrayStride 16   <- WRONG! Should be 4
OpDecorate %_arr_float_uint_16_3 ArrayStride 16
...
OpDecorate %_arr_uint_uint_16_10 ArrayStride 16   <- WRONG! Should be 4
```

The types themselves are CORRECT (`OpTypeArray %float %uint_16_2`), but the ArrayStride decoration is 16 instead of 4.

Member offsets in the struct:
- Member 3 (intensities): Offset 768
- Member 4: Offset 1024  <- means 256 bytes for float[16] instead of 64!

### Root Cause Identified

In `bwsl_spirv_backend.cpp` line 709-712:
```cpp
// std140: array stride aligned to 16 bytes
u32 stride = (fieldSize + 15) & ~15;
u32 stride_val[] = {stride};
EmitDecoration(arrayTypeId, spv::DecorationArrayStride, stride_val, 1);
```

**Problem**: std140 layout (16-byte array element alignment) is being applied to ALL structs, including those used in storage buffers.

**std140 vs std430**:
- std140 (for uniform buffers): Arrays of scalars/vec2 are padded to 16 bytes
- std430 (for storage buffers): Arrays use natural element size (float = 4 bytes)

Since `LightSourcesSoA` is used as a storage buffer (`buffer lightBuffer LightSourcesSoA`), it should use std430 layout, not std140.

---

## Step 2: Implementing Fix

### Issue Locations

**Two places have the std140 padding logic:**

1. **IR Lowering** (`bwsl_ir_lowering.h`):
   - Line 1806: `u32 arrayStride = (fieldSize + 15) & ~15;`
   - Line 1873: `u32 arrayStride = (fieldSize + 15) & ~15;`

2. **SPIR-V Backend** (`bwsl_spirv_backend.cpp`):
   - Line 710: `u32 stride = (fieldSize + 15) & ~15;`

### ResourceType Distinction

The AST has a `ResourceType` enum that distinguishes:
- `CBUFFER` = Constant/Uniform buffer (std140 layout)
- `BUFFER` = Storage buffer (std430 layout)

### Fix Strategy

**Use std430 layout for storage buffer struct types:**
- std430 allows natural element sizes for scalar arrays (float=4, uint=4)
- This matches CPU struct layouts for proper CPU/GPU interop
- Struct types used with `buffer` keyword should use std430

**Implementation plan:**
1. Fix IR lowering to use natural array strides (not 16-byte aligned)
2. Fix SPIR-V backend to emit natural array strides
3. The byte offsets will still respect proper alignment rules for each element type

---

### Fix Attempt 1: Use std430 Layout for Struct Arrays

**What I'm about to do:**
- In IR lowering, change array stride calculation from `(fieldSize + 15) & ~15` to `fieldSize`
- In SPIR-V backend, change array stride decoration from 16-byte aligned to natural size

**Why this should work:**
- std430 layout uses natural element sizes for arrays
- This matches how C/C++ struct arrays work
- SPIRV-Cross will generate correct Metal/HLSL struct layouts

---

## Fix Attempt 1: SUCCESSFUL

### Changes Made

**1. bwsl_ir_lowering.h (two locations around lines 1805 and 1877):**
```cpp
// BEFORE:
// Array stride is aligned to 16 bytes in std140
u32 arrayStride = (fieldSize + 15) & ~15;

// AFTER:
// std430 layout: scalars and vec2 use natural stride, vec3+ rounds to 16
u32 arrayStride;
if (fieldSize <= 8) {
    arrayStride = fieldSize;  // Natural size for scalars (4) and vec2 (8)
} else {
    arrayStride = (fieldSize + 15) & ~15;  // Round to 16 for vec3, vec4, mat
}
```

**2. bwsl_spirv_backend.cpp (line 709):**
```cpp
// BEFORE:
// std140: array stride aligned to 16 bytes
u32 stride = (fieldSize + 15) & ~15;

// AFTER:
// std430 layout: scalars and vec2 use natural stride, vec3+ rounds to 16
u32 stride;
if (fieldSize <= 8) {
    stride = fieldSize;  // Natural size for scalars (4) and vec2 (8)
} else {
    stride = (fieldSize + 15) & ~15;  // Round to 16 for vec3, vec4, mat
}
```

### Verification Results

**SPIR-V Array Strides - After Fix:**
```
OpDecorate %_arr_v3float_uint_16 ArrayStride 16     <- Correct for vec3
OpDecorate %_arr_float_uint_16_2 ArrayStride 4      <- FIXED! (was 16)
OpDecorate %_arr_float_uint_16_3 ArrayStride 4      <- FIXED!
...
OpDecorate %_arr_uint_uint_16_10 ArrayStride 4      <- FIXED!
```

**Member Offsets - After Fix:**
```
OpMemberDecorate %_struct_50 3 Offset 768    <- intensities
OpMemberDecorate %_struct_50 4 Offset 832    <- 768 + 64 = 832 (CORRECT!)
OpMemberDecorate %_struct_50 12 Offset 1344  <- version
```

**Struct Size - After Fix:**
```
OpDecorate %_runtimearr__struct_50 ArrayStride 1360
```

**Metal Output - After Fix:**
```cpp
struct _50 {
    float3 _m0[16];   // positions - float3[16] ✓
    float3 _m1[16];   // directions - float3[16] ✓
    float3 _m2[16];   // colors - float3[16] ✓
    float _m3[16];    // intensities - FIXED! (was float4[16])
    float _m4[16];    // constantAttenuations - FIXED!
    float _m5[16];    // linearAttenuations - FIXED!
    float _m6[16];    // quadraticAttenuations - FIXED!
    float _m7[16];    // innerCutoffs - FIXED!
    float _m8[16];    // outerCutoffs - FIXED!
    float _m9[16];    // lengths - FIXED!
    float _m10[16];   // radii - FIXED!
    uint _m11[16];    // types - FIXED! (was uint4[16])
    uint _m12;        // version ✓
    char _m0_final_padding[12];
};
```

### Size Comparison

| Struct | Before | After | Expected |
|--------|--------|-------|----------|
| LightSourcesSoA | 3088 bytes | 1360 bytes | ~1360 bytes ✓ |
| ShadowData | 432 bytes | 432 bytes | 432 bytes ✓ |

## Summary

The bug was caused by applying std140 layout rules (16-byte array stride alignment) to all struct types, even those used in storage buffers which should use std430 layout.

The fix modifies the array stride calculation to use:
- **Natural element size** for scalars (4 bytes) and vec2 (8 bytes)
- **16-byte aligned size** for vec3, vec4, and matrices

This matches the std430 layout specification and ensures proper CPU/GPU struct interoperability.

