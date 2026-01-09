# Progress: Fix Arrays in Structs for SPIR-V Backend

## Problem
BWSL doesn't properly support arrays inside structs. When compiling `World.bwsl`, structs generate scalar types instead of array types in SPIR-V.

Example: `LightSourcesSoA` with `float3[16] positions` generates scalar `%v3float` members instead of `%_arr_v3float_16`.

## Root Cause Analysis

### Issue 1: Struct Type Generation (FIXED)
**Location:** `GetStructTypeId()` in `bwsl_spirv_backend.cpp` (lines 632-706)

The function ignored `ir->structFieldArraySizes` when building struct types. Fixed by:
- Checking `ir->structFieldArraySizes[structInfo->fieldOffset + i]` for each field
- If non-zero, creating `OpTypeArray` wrapping the base type
- Adding `ArrayStride` decoration (std140: 16-byte aligned)
- Adding `ColMajor` and `MatrixStride` decorations for matrix fields

### Issue 2: Zero Struct Initialization (FIXED)
**Location:** `EmitZeroStruct()` in `bwsl_ir_lowering.h` (lines 1573-1608)

The function created scalar zero values for array fields, causing type mismatches. Fixed by:
- Checking `program.structFieldArraySizes[structInfo->fieldOffset + f]`
- Skipping zero initialization for array fields (they remain undefined)

### Issue 3: Struct Field Extraction (FIXED)
**Location:** `OP_STRUCT_EXTRACT` case in `bwsl_spirv_backend.cpp` (lines 3053-3133)

The result type was determined from the element type, not the array type. Fixed by:
- Looking up cached field type IDs from `structFieldTypeIds[]` instead of computing fresh
- This ensures the extract result type matches the struct's field type (array if field is array)

### Issue 4: Array Element Access - Type Propagation (FIXED)
**Location:** `OP_ARRAY_LOAD` case in `bwsl_spirv_backend.cpp` (lines 3383-3560)

When accessing `skel.bones[0]`:
1. `OP_STRUCT_EXTRACT` returns an array of mat4 (correct after fix)
2. `OP_ARRAY_LOAD` tries to do matrix column extraction because `ir->registerTypes[base_reg]` is `MAT4`
3. The extraction produces `mat4v4float` (correct), but downstream code expects `v4float`

**Solution:** Added `spirvTypeOverrides` tracking array to override IR type system for registers where the actual SPIR-V type differs from what the IR thinks.

## Files Modified

### bwsl_spirv_backend.h
- Added `regIsStructArrayField` tracking array for struct field array values
- Added `spirvTypeOverrides` array to store actual SPIR-V type IDs when they differ from IR types

### bwsl_spirv_backend.cpp
1. **Initialization (~line 296-302):**
   - Added allocation for `regIsStructArrayField`
   - Added allocation for `spirvTypeOverrides`

2. **GetResultType (~line 1014-1020):**
   - Check `spirvTypeOverrides[dest_reg]` first before falling back to IR type system
   - If override exists, use it directly

3. **OP_LOAD_REG (~lines 1194-1239):**
   - Check `spirvTypeOverrides[src_reg]` when emitting OpCopyObject
   - Propagate type override from source to destination register

4. **OP_STORE_REG (~lines 1282-1302):**
   - Check `spirvTypeOverrides[src_reg]` when emitting OpCopyObject for preallocated IDs
   - Propagate type override from source to destination register

5. **GetStructTypeId (~lines 632-706):**
   - Check `ir->structFieldArraySizes` for each field
   - Create `OpTypeArray` with proper stride for array fields
   - Add `ColMajor`/`MatrixStride` decorations for matrix fields

6. **OP_STRUCT_EXTRACT (~lines 3069-3133):**
   - Use cached `structFieldTypeIds[]` for result type
   - Set `regIsStructArrayField[dest_reg]` when extracting array field

7. **OP_ARRAY_LOAD (~lines 3455-3541):**
   - Check `regIsStructArrayField[base_reg]` first
   - If true, do array element extraction with correct element type
   - Set `spirvTypeOverrides[dest_reg]` to the actual element type
   - Otherwise fall through to matrix/vector handling

8. **OP_ARRAY_STORE (~lines 3726-3782):**
   - Check `spirvTypeOverrides[value_reg]` first for non-storage arrays
   - Use the override type for OpCopyObject if available

### bwsl_ir_lowering.h
1. **EmitZeroStruct (~lines 1582-1591):**
   - Check `program.structFieldArraySizes` for each field
   - Skip zero initialization for array fields

## Key Insight
The fundamental issue was that BWSL's IR type system (`CoreType`) doesn't distinguish between:
- `mat4` (a single matrix)
- `mat4[]` (an array of matrices)

Both are represented as `CoreType::MAT4` in `ir->registerTypes`. The solution was to:
1. Track when a register holds a struct field array via `regIsStructArrayField`
2. When extracting an element from such an array, store the actual SPIR-V type ID in `spirvTypeOverrides`
3. Check `spirvTypeOverrides` in all code paths that emit `OpCopyObject` or determine types

### Issue 5: Storage Buffer Array Field Access (FIXED)
**Location:** `OP_STRUCT_EXTRACT` and `OP_ARRAY_LOAD` in `bwsl_spirv_backend.cpp`

When accessing array fields from storage buffers (e.g., `shadow.cascades[i]`), needed:
- Track storage class via `storagePtrStorageClass[dest_reg]`
- Track element type via `storagePtrElemTypes[dest_reg]`
- Use `OpAccessChain + OpLoad` instead of `OpCompositeExtract` for storage buffer access

### Issue 6: Element Type Propagation Through Copies (FIXED)
**Location:** `OP_LOAD_REG` and `OP_STORE_REG` in `bwsl_spirv_backend.cpp`

Array field tracking wasn't propagated through register copies. Fixed by:
- Propagating `regIsStructArrayField` in LOAD_REG and STORE_REG
- Propagating `storagePtrStorageClass` in LOAD_REG and STORE_REG
- Propagating `storagePtrElemTypes` in LOAD_REG and STORE_REG

### Issue 7: Non-Storage Struct Array Element Type (FIXED)
**Location:** `OP_STRUCT_EXTRACT` in `bwsl_spirv_backend.cpp`

When extracting array fields from non-storage structs, element type wasn't tracked.
Fixed by setting `storagePtrElemTypes[dest_reg]` for ALL array field extractions, not just storage buffers.

## Test Files
- `tests/structs_advanced.bwsl` - mat4[4], float[4], Transform[4] - **PASSES**
- `tests/structs_array_member_access.bwsl` - Pose[2], Frame[2] (nested) - **PASSES**
- `tests/from_engine/World.bwsl` - CascadeData[4], mat4 in nested struct
  - Struct array access now works correctly
  - **BLOCKED BY SEPARATE ISSUE**: User-defined functions (e.g., `sampleCascadedShadow`, `sampleShadowPCF`) emit `OpUndef` because function inlining isn't implemented

## Status
**RESOLVED** - The arrays-in-structs bug is fixed:
1. Struct type generation correctly creates array types for array fields
2. Element type tracking works for both storage buffer and non-storage struct array fields
3. Type propagation through register copies preserves tracking
4. All dedicated struct array tests pass

**SEPARATE ISSUE**: World.bwsl fails due to unimplemented function inlining (OP_CALL emits OpUndef). This is not related to the array-in-structs bug.
