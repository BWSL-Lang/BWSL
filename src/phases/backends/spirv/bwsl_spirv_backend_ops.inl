// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Main IR instruction translation dispatch and opcode emission.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

void SPIRVBuilder::TranslateInstruction(u32 ir_idx) {
  IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[ir_idx]);
  spv::Op spv_op = IRToSpvOp(op);

  u32 dest = GetSpirvId(ir->destinations[ir_idx]);

  // Ensure the instruction result type exists in the type section. Individual
  // cases still compute the concrete result type they need for emission.
  CoreType instType = static_cast<CoreType>(ir->types[ir_idx]);
  if (instType == CoreType::CUSTOM || instType == CoreType::ENUM) {
    u16 dest_reg = ir->destinations[ir_idx];
    if (dest_reg < 512 && ir->registerStructTypes) {
      u32 structHash = ir->registerStructTypes[dest_reg];
      if (structHash != 0) {
        (void)GetStructTypeId(structHash);
      }
    }
  } else {
    (void)GetTypeId(instType);
  }

  switch (op) {
  // ========== No-ops and pass-through ==========
  case IR::OP_NOP: {
    // No operation - but if it has a destination register, emit OpUndef
    // This handles placeholders for unimplemented features
    u16 dest_reg = ir->destinations[ir_idx];
    if (dest_reg != 0 && dest_reg != 0xFFFF) {
      CoreType resultType = static_cast<CoreType>(ir->types[ir_idx]);
      if (resultType == CoreType::INVALID || resultType == CoreType::VOID) {
        resultType = CoreType::FLOAT; // Default fallback
      }
      u32 result_type_id = GetTypeId(resultType);
      if (result_type_id != 0) {
        Emit(spv::OpUndef, result_type_id, dest);
      }
    }
    break;
  }

  case IR::OP_CALL: {
    // Function call - for now emit OpUndef as placeholder since inlining isn't
    // implemented
    // TODO: Implement function inlining during IR lowering
    // The destination register needs a valid SPIR-V ID that's actually defined
    CoreType resultType = static_cast<CoreType>(ir->types[ir_idx]);
    if (resultType == CoreType::INVALID || resultType == CoreType::VOID) {
      resultType =
          CoreType::FLOAT3; // Default for position decompression functions
    }
    u32 result_type_id = GetTypeId(resultType);
    // OpUndef: result_type result_id
    Emit(spv::OpUndef, result_type_id, dest);
    break;
  }

  case IR::OP_LOAD_REG: {
    // Copy register to register
    u16 src_reg = ir->GetOperand(ir_idx, 0);
    u32 src_id = GetSpirvId(src_reg);
    u16 dest_reg = ir->destinations[ir_idx];

    // Check if source is a constant (float 0x8000, int 0x4000, bool 0xC000)
    bool srcIsConstant = (src_reg & 0xC000) != 0;
    bool needsPreallocDef =
        (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]);

    if ((srcIsConstant || needsPreallocDef) && dest_reg < idCapacity) {
      // For constants, we must emit OpCopyObject to create a properly defined
      // ID. This is critical for phi nodes: the phi may be emitted before this
      // block, so the destination register needs an actual instruction defining
      // it.
      u32 type_id = 0;

      // Check if source register has a SPIR-V type override (e.g., from struct
      // array extraction)
      if (!srcIsConstant && src_reg < idCapacity &&
          spirvTypeOverrides[src_reg] != 0) {
        type_id = spirvTypeOverrides[src_reg];
        // Propagate the override to the destination
        spirvTypeOverrides[dest_reg] = type_id;
      }

      if (type_id == 0) {
        CoreType destType = CoreType::FLOAT;
        if (dest_reg < ir->registerCount && ir->registerTypes) {
          destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
        }
        // Infer type from constant encoding if register type not set
        if (destType == CoreType::VOID || destType == CoreType::INVALID) {
          if ((src_reg & 0xC000) == 0xC000) {
            destType = CoreType::BOOL;
          } else if (src_reg & 0x8000) {
            destType = CoreType::FLOAT;
          } else {
            destType = CoreType::INT;
          }
        }
        if ((destType == CoreType::CUSTOM || destType == CoreType::ENUM) &&
            ir->registerStructTypes && dest_reg < ir->registerCount) {
          u32 structHash = ir->registerStructTypes[dest_reg];
          if (structHash != 0) {
            type_id = GetStructTypeId(structHash);
          }
        } else {
          type_id = GetTypeId(destType);
        }
        if (type_id == 0) {
          type_id = GetTypeId(CoreType::FLOAT);
        }
      }
      Emit(spv::OpCopyObject, type_id, dest, src_id);
      if (needsPreallocDef) {
        hasPreAllocatedId[dest_reg] = false;
      }
      // Propagate storage pointer tracking for OpCopyObject path
      if (!srcIsConstant && src_reg < idCapacity) {
        if (regIsStructArrayField[src_reg]) {
          regIsStructArrayField[dest_reg] = true;
        }
        if (storagePtrStorageClass[src_reg] != 0) {
          storagePtrStorageClass[dest_reg] = storagePtrStorageClass[src_reg];
        }
        if (storagePtrElemTypes[src_reg] != 0) {
          storagePtrElemTypes[dest_reg] = storagePtrElemTypes[src_reg];
        }
      }
    } else if (dest_reg < idCapacity) {
      // For register-to-register copy, just alias the SPIR-V ID
      spirvIds[dest_reg] = src_id;
      // Propagate type override if source has one
      if (src_reg < idCapacity && spirvTypeOverrides[src_reg] != 0) {
        spirvTypeOverrides[dest_reg] = spirvTypeOverrides[src_reg];
      }
      // Propagate storage pointer tracking
      if (src_reg < idCapacity) {
        if (regIsStructArrayField[src_reg]) {
          regIsStructArrayField[dest_reg] = true;
        }
        if (storagePtrStorageClass[src_reg] != 0) {
          storagePtrStorageClass[dest_reg] = storagePtrStorageClass[src_reg];
        }
        if (storagePtrElemTypes[src_reg] != 0) {
          storagePtrElemTypes[dest_reg] = storagePtrElemTypes[src_reg];
        }
      }
    }
    break;
  }

  case IR::OP_LOAD_CONST: {
    // Load constant - the constant value is in metadata or operands
    u16 dest_reg = ir->destinations[ir_idx];
    // The constant index/value is typically in operand[0]
    u16 const_ref = ir->GetOperand(ir_idx, 0);
    u32 const_id = GetSpirvId(const_ref);
    if (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]) {
      // Define the pre-allocated ID so forward-referenced PHIs stay valid.
      u32 type_id = GetResultType(dest_reg, const_ref);
      Emit(spv::OpCopyObject, type_id, dest, const_id);
      hasPreAllocatedId[dest_reg] = false;
    } else if (dest_reg < idCapacity) {
      // Map dest register to constant ID
      spirvIds[dest_reg] = const_id;
    }
    break;
  }

  case IR::OP_STORE_REG: {
    // Store to a register (variable assignment)
    // In SSA form, this is just aliasing: dest = src
    //
    // Special handling for PHI operands: if this register was pre-allocated
    // an ID because a PHI references it from a not-yet-processed block,
    // we need to define that ID with OpCopyObject. After defining it,
    // we clear the flag so subsequent STORE_REG to the same register
    // just aliases without emitting another definition.
    u16 dest_reg = ir->destinations[ir_idx];
    u16 src_reg = ir->GetOperand(ir_idx, 0);
    u32 src_id = GetSpirvId(src_reg);

    if (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]) {
      // This register has a pre-allocated ID that needs to be defined
      u32 existing_id = spirvIds[dest_reg];
      if (existing_id != src_id) {
        // Define the pre-allocated ID by copying from the source
        // Check for type override on source first
        u32 type_id = 0;
        if (src_reg < idCapacity && spirvTypeOverrides[src_reg] != 0) {
          type_id = spirvTypeOverrides[src_reg];
          spirvTypeOverrides[dest_reg] = type_id;
        } else {
          type_id = GetResultType(dest_reg, src_reg);
        }
        Emit(spv::OpCopyObject, type_id, existing_id, src_id);
      }
      // Clear the flag - ID is now defined, subsequent STORE_REG will just
      // alias
      hasPreAllocatedId[dest_reg] = false;
      // Keep the pre-allocated ID in spirvIds so PHIs can reference it
      // Propagate storage pointer tracking
      if (src_reg < idCapacity) {
        if (regIsStructArrayField[src_reg]) {
          regIsStructArrayField[dest_reg] = true;
        }
        if (storagePtrStorageClass[src_reg] != 0) {
          storagePtrStorageClass[dest_reg] = storagePtrStorageClass[src_reg];
        }
        if (storagePtrElemTypes[src_reg] != 0) {
          storagePtrElemTypes[dest_reg] = storagePtrElemTypes[src_reg];
        }
      }
    } else if (dest_reg < idCapacity) {
      // No pre-allocated ID (or already defined), just alias
      spirvIds[dest_reg] = src_id;
      // Propagate type override if source has one
      if (src_reg < idCapacity && spirvTypeOverrides[src_reg] != 0) {
        spirvTypeOverrides[dest_reg] = spirvTypeOverrides[src_reg];
      }
      // Propagate storage pointer tracking
      if (src_reg < idCapacity) {
        if (regIsStructArrayField[src_reg]) {
          regIsStructArrayField[dest_reg] = true;
        }
        if (storagePtrStorageClass[src_reg] != 0) {
          storagePtrStorageClass[dest_reg] = storagePtrStorageClass[src_reg];
        }
        if (storagePtrElemTypes[src_reg] != 0) {
          storagePtrElemTypes[dest_reg] = storagePtrElemTypes[src_reg];
        }
      }
    }
    break;
  }

  // ========== Arithmetic ==========
  case IR::OP_FADD:
  case IR::OP_FSUB:
  case IR::OP_FDIV:
  case IR::OP_FMOD:
  case IR::OP_FREM: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Check for scalar-vector mixing which requires splatting
    // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are
    // scalars
    bool op1_is_scalar = (op1_reg & 0xE000) != 0;
    bool op2_is_scalar = (op2_reg & 0xE000) != 0;

    CoreType destType = CoreType::FLOAT;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
    }

    // Check register types for non-constant operands
    if (ir->registerTypes) {
      if (!op1_is_scalar && op1_reg < ir->registerCount) {
        CoreType op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        op1_is_scalar =
            (op1_type == CoreType::FLOAT || op1_type == CoreType::INT ||
             op1_type == CoreType::UINT);
      }
      if (!op2_is_scalar && op2_reg < ir->registerCount) {
        CoreType op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        op2_is_scalar =
            (op2_type == CoreType::FLOAT || op2_type == CoreType::INT ||
             op2_type == CoreType::UINT);
      }
    }

    u32 result_type =
        GetResultType(dest_reg, op1_is_scalar ? op2_reg : op1_reg);

    // If one operand is scalar and result is vector, we need to splat
    bool resultIsVector =
        (destType == CoreType::FLOAT2 || destType == CoreType::FLOAT3 ||
         destType == CoreType::FLOAT4 || destType == CoreType::INT2 ||
         destType == CoreType::INT3 || destType == CoreType::INT4);

    if (resultIsVector) {
      u32 numComponents =
          (destType == CoreType::FLOAT2 || destType == CoreType::INT2)   ? 2
          : (destType == CoreType::FLOAT3 || destType == CoreType::INT3) ? 3
                                                                         : 4;

      if (op1_is_scalar && !op2_is_scalar) {
        // Splat op1 to a vector
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = result_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op1;
        }
        op1 = splatted;
      } else if (op2_is_scalar && !op1_is_scalar) {
        // Splat op2 to a vector
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = result_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op2;
        }
        op2 = splatted;
      }
    }

    Emit(spv_op, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_FMUL: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Load from storage pointers if needed
    if (op1_reg < idCapacity && storagePtrStorageClass[op1_reg] != 0) {
      CoreType loadType =
          ir->registerTypes ? static_cast<CoreType>(ir->registerTypes[op1_reg])
                            : CoreType::FLOAT;
      u32 load_type = GetTypeId(loadType);
      u32 loaded = AllocateId();
      Emit(spv::OpLoad, load_type, loaded, op1);
      op1 = loaded;
    }
    if (op2_reg < idCapacity && storagePtrStorageClass[op2_reg] != 0) {
      CoreType loadType =
          ir->registerTypes ? static_cast<CoreType>(ir->registerTypes[op2_reg])
                            : CoreType::FLOAT;
      u32 load_type = GetTypeId(loadType);
      u32 loaded = AllocateId();
      Emit(spv::OpLoad, load_type, loaded, op2);
      op2 = loaded;
    }

    u32 result_type = GetResultType(dest_reg, op1_reg);

    // Check for vector-scalar multiplication
    // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are always
    // scalars
    bool op1_is_scalar = (op1_reg & 0xE000) != 0;
    bool op2_is_scalar = (op2_reg & 0xE000) != 0;

    // Check register types independently for each non-constant operand
    if (ir->registerTypes) {
      if (!op1_is_scalar && op1_reg < ir->registerCount) {
        CoreType op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        op1_is_scalar =
            (op1_type == CoreType::FLOAT || op1_type == CoreType::INT ||
             op1_type == CoreType::UINT);
      }
      if (!op2_is_scalar && op2_reg < ir->registerCount) {
        CoreType op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        op2_is_scalar =
            (op2_type == CoreType::FLOAT || op2_type == CoreType::INT ||
             op2_type == CoreType::UINT);
      }
    }

    if (op2_is_scalar && !op1_is_scalar) {
      // Vector * Scalar -> OpVectorTimesScalar
      Emit(spv::OpVectorTimesScalar, result_type, dest, op1, op2);
    } else if (op1_is_scalar && !op2_is_scalar) {
      // Scalar * Vector -> swap and use OpVectorTimesScalar
      Emit(spv::OpVectorTimesScalar, result_type, dest, op2, op1);
    } else {
      // Both vectors or both scalars -> OpFMul
      Emit(spv::OpFMul, result_type, dest, op1, op2);
    }
    break;
  }

  case IR::OP_IADD:
  case IR::OP_ISUB:
  case IR::OP_IMUL:
  case IR::OP_IDIV:
  case IR::OP_IMOD: {
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
    u32 result_type = GetTypeId(CoreType::INT);
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        result_type = GetTypeId(regType);
      }
    }
    Emit(spv_op, result_type, dest, op1, op2);
    break;
  }

  // ========== Matrix Operations ==========
  case IR::OP_MAT_MUL: {
    // Matrix * Matrix
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
    u32 result_type = GetTypeId(CoreType::MAT4); // Default to mat4
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType == CoreType::MAT2 || regType == CoreType::MAT3 ||
          regType == CoreType::MAT4) {
        result_type = GetTypeId(regType);
      }
    }
    Emit(spv::OpMatrixTimesMatrix, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_MAT_VEC_MUL: {
    // Matrix * Vector -> Vector
    // SPIR-V requires: vector components == matrix columns
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0); // Matrix
    u16 op2_reg = ir->GetOperand(ir_idx, 1); // Vector
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Get destination type to determine expected dimensions
    // For square matrices, result vec size == matrix columns == matrix rows
    CoreType destType = CoreType::FLOAT4;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
    }

    u32 expectedSize = 4;
    if (destType == CoreType::FLOAT2)
      expectedSize = 2;
    else if (destType == CoreType::FLOAT3)
      expectedSize = 3;

    // Get input vector type - check both constant flags and register types
    CoreType vecType = CoreType::FLOAT4; // Default assumption
    if (!(op2_reg & 0xE000)) {
      // Not a constant (0x8000=float, 0x4000=int, 0x2000=uint), check register
      // type
      if (ir->registerTypes && op2_reg < ir->registerCount) {
        CoreType regType = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        if (regType == CoreType::FLOAT2 || regType == CoreType::FLOAT3 ||
            regType == CoreType::FLOAT4) {
          vecType = regType;
        }
      }
    }

    u32 vecSize = 4;
    if (vecType == CoreType::FLOAT2)
      vecSize = 2;
    else if (vecType == CoreType::FLOAT3)
      vecSize = 3;

    // If result is smaller than vec4, we may need to truncate
    // This handles cases where the IR type info is missing or incorrect
    if (expectedSize < 4 || vecSize > expectedSize) {
      CoreType targetVecType = destType; // Match destination type
      u32 target_vec_type_id = GetTypeId(targetVecType);

      // Truncate: extract first N components using VectorShuffle
      u32 truncated = AllocateId();
      u32 wordCount = 5 + expectedSize; // OpVectorShuffle base + indices
      if (currentFunctionSize + wordCount > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (wordCount << 16) | spv::OpVectorShuffle;
      currentFunction[currentFunctionSize++] = target_vec_type_id;
      currentFunction[currentFunctionSize++] = truncated;
      currentFunction[currentFunctionSize++] = op2;
      currentFunction[currentFunctionSize++] = op2;
      for (u32 i = 0; i < expectedSize; i++) {
        currentFunction[currentFunctionSize++] = i;
      }
      op2 = truncated;
    }

    // Result type matches the destination register type
    u32 result_type = GetTypeId(destType);
    Emit(spv::OpMatrixTimesVector, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_VEC_MAT_MUL: {
    // Vector * Matrix -> Vector
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0)); // Vector
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1)); // Matrix
    // Result type is the vector type
    u32 result_type = GetTypeId(CoreType::FLOAT4); // Default to vec4
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType == CoreType::FLOAT2 || regType == CoreType::FLOAT3 ||
          regType == CoreType::FLOAT4) {
        result_type = GetTypeId(regType);
      }
    }
    Emit(spv::OpVectorTimesMatrix, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_MAT_SCALE: {
    // Matrix * Scalar -> Matrix
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0)); // Matrix
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1)); // Scalar
    u32 result_type = GetTypeId(CoreType::MAT4);     // Default to mat4
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType == CoreType::MAT2 || regType == CoreType::MAT3 ||
          regType == CoreType::MAT4) {
        result_type = GetTypeId(regType);
      }
    }
    Emit(spv::OpMatrixTimesScalar, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_MAT_TRANSPOSE: {
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::MAT4);
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType == CoreType::MAT2 || regType == CoreType::MAT3 ||
          regType == CoreType::MAT4) {
        result_type = GetTypeId(regType);
      }
    }
    Emit(spv::OpTranspose, result_type, dest, op1);
    break;
  }

  case IR::OP_MAT_INVERSE: {
    u16 dest_reg = ir->destinations[ir_idx];
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::MAT4);
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType == CoreType::MAT2 || regType == CoreType::MAT3 ||
          regType == CoreType::MAT4) {
        result_type = GetTypeId(regType);
      }
    }
    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = GLSLstd450MatrixInverse;
    currentFunction[currentFunctionSize++] = op1;
    break;
  }

  case IR::OP_MAT_DET: {
    // Determinant returns a scalar float regardless of matrix
    // dimension — the generic 1-operand ext-inst path used the
    // operand's matrix type as the result type, which failed
    // validation downstream because the consumer expected a float.
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::FLOAT);
    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = GLSLstd450Determinant;
    currentFunction[currentFunctionSize++] = op1;
    break;
  }

  // ========== Bitwise/Logical Operations ==========
  // Note: For bitwise ops, int constants need to match the type of the other
  // operand For booleans, use OpLogicalAnd/OpLogicalOr instead of bitwise ops
  case IR::OP_AND: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, op1_reg);

    CoreType op1_type = GetOperandType(op1_reg);

    // Booleans require OpLogicalAnd, integers use OpBitwiseAnd. Vector
    // bool (bvec2/3/4) operands also go through the logical path — SPIR-V
    // rejects OpBitwiseAnd on a bvec result type.
    bool isBool = (op1_type == CoreType::BOOL || op1_type == CoreType::BOOL2 ||
                   op1_type == CoreType::BOOL3 || op1_type == CoreType::BOOL4);
    if (isBool) {
      u32 op1 = GetSpirvId(op1_reg);
      u32 op2 = GetSpirvId(op2_reg);
      Emit(spv::OpLogicalAnd, result_type, dest, op1, op2);
    } else {
      bool useUint = (op1_type == CoreType::UINT);
      u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
      u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
      Emit(spv::OpBitwiseAnd, result_type, dest, op1, op2);
    }
    break;
  }

  case IR::OP_OR: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, op1_reg);
    CoreType op1_type = GetOperandType(op1_reg);

    // Booleans require OpLogicalOr, integers use OpBitwiseOr. Vector
    // bool (bvec2/3/4) operands also go through the logical path.
    bool isBool = (op1_type == CoreType::BOOL || op1_type == CoreType::BOOL2 ||
                   op1_type == CoreType::BOOL3 || op1_type == CoreType::BOOL4);
    if (isBool) {
      u32 op1 = GetSpirvId(op1_reg);
      u32 op2 = GetSpirvId(op2_reg);
      Emit(spv::OpLogicalOr, result_type, dest, op1, op2);
    } else {
      bool useUint = (op1_type == CoreType::UINT);
      u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
      u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
      Emit(spv::OpBitwiseOr, result_type, dest, op1, op2);
    }
    break;
  }

  case IR::OP_XOR: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, op1_reg);
    CoreType op1_type =
        static_cast<CoreType>(ir->registerTypes[op1_reg & 0x3FFF]);
    bool useUint = (op1_type == CoreType::UINT);
    u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
    u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
    Emit(spv::OpBitwiseXor, result_type, dest, op1, op2);
    break;
  }

  case IR::OP_NOT: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    CoreType op_type = GetOperandType(op_reg);

    // Booleans require OpLogicalNot, integers use OpNot (bitwise)
    if (op_type == CoreType::BOOL || op_type == CoreType::BOOL2 ||
        op_type == CoreType::BOOL3 || op_type == CoreType::BOOL4) {
      Emit(spv::OpLogicalNot, result_type, dest, operand);
    } else {
      Emit(spv::OpNot, result_type, dest, operand);
    }
    break;
  }

  // ========== Unary Negation ==========
  case IR::OP_FNEG: {
    // Float negation: -x
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    CoreType opType = GetOperandType(op_reg);
    // OpFNegate only works on scalar/vector float. For matrices, multiply
    // by -1.0 via OpMatrixTimesScalar.
    if (opType == CoreType::MAT2 || opType == CoreType::MAT3 ||
        opType == CoreType::MAT4) {
      u32 neg_one = GetFloatConstantId(-1.0f);
      Emit(spv::OpMatrixTimesScalar, result_type, dest, operand, neg_one);
    } else {
      Emit(spv::OpFNegate, result_type, dest, operand);
    }
    break;
  }

  case IR::OP_INEG: {
    // Integer negation: -x
    u16 dest_reg = ir->destinations[ir_idx];
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
    Emit(spv::OpSNegate, result_type, dest, operand);
    break;
  }

  // ========== Derivatives (Fragment only) ==========
  case IR::OP_DDX:
  case IR::OP_DDY:
  case IR::OP_DDX_FINE:
  case IR::OP_DDY_FINE:
  case IR::OP_DDX_COARSE:
  case IR::OP_DDY_COARSE:
  case IR::OP_FWIDTH:
  case IR::OP_FWIDTH_FINE:
  case IR::OP_FWIDTH_COARSE: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    spv::Op spv_op2 = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
    Emit(spv_op2, result_type, dest, operand);
    break;
  }

  // any(bvec) / all(bvec) — reduce a bool vector to a scalar bool.
  case IR::OP_ANY:
  case IR::OP_ALL: {
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetTypeId(CoreType::BOOL);
    spv::Op spv_op2 = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
    Emit(spv_op2, result_type, dest, operand);
    break;
  }

  // isnan(x) / isinf(x) — result type is bool scalar or bvecN matching the
  // input vector width. IR lowering already set the dest register's type
  // correctly; fetch it via GetResultType so matching works for all widths.
  case IR::OP_ISNAN:
  case IR::OP_ISINF: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    spv::Op spv_op2 = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
    Emit(spv_op2, result_type, dest, operand);
    break;
  }

  case IR::OP_ISFINITE: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);

    u32 is_nan = AllocateId();
    u32 is_inf = AllocateId();
    u32 nan_or_inf = AllocateId();
    Emit(spv::OpIsNan, result_type, is_nan, operand);
    Emit(spv::OpIsInf, result_type, is_inf, operand);
    Emit(spv::OpLogicalOr, result_type, nan_or_inf, is_nan, is_inf);
    Emit(spv::OpLogicalNot, result_type, dest, nan_or_inf);
    break;
  }

  case IR::OP_ISNORMAL: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    CoreType op_type = GetOperandType(op_reg);
    u32 operand_type = GetTypeId(op_type);

    u32 min_normal_scalar = GetFloatConstantId(1.17549435e-38f);
    u32 min_normal = min_normal_scalar;
    u32 component_count = 1;
    switch (op_type) {
    case CoreType::FLOAT2:
      component_count = 2;
      break;
    case CoreType::FLOAT3:
      component_count = 3;
      break;
    case CoreType::FLOAT4:
      component_count = 4;
      break;
    default:
      break;
    }
    if (component_count > 1) {
      u32 constituents[4] = {min_normal_scalar, min_normal_scalar,
                             min_normal_scalar, min_normal_scalar};
      min_normal =
          GetCompositeConstantId(operand_type, constituents, component_count);
    }

    u32 abs_value = AllocateId();
    Emit(spv::OpExtInst, operand_type, abs_value, glslStd450Id, GLSLstd450FAbs,
         operand);

    u32 normal_magnitude = AllocateId();
    u32 is_inf = AllocateId();
    u32 not_inf = AllocateId();
    Emit(spv::OpFOrdGreaterThanEqual, result_type, normal_magnitude, abs_value,
         min_normal);
    Emit(spv::OpIsInf, result_type, is_inf, operand);
    Emit(spv::OpLogicalNot, result_type, not_inf, is_inf);
    Emit(spv::OpLogicalAnd, result_type, dest, normal_magnitude, not_inf);
    break;
  }

  // ========== Shift Operations ==========
  case IR::OP_SHL: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 shift_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, base_reg);
    CoreType base_type =
        static_cast<CoreType>(ir->registerTypes[base_reg & 0x3FFF]);
    bool useUint = (base_type == CoreType::UINT);
    u32 base = GetSpirvIdForBitwise(base_reg, useUint);
    u32 shift = GetSpirvIdForBitwise(shift_reg, useUint);
    Emit(spv::OpShiftLeftLogical, result_type, dest, base, shift);
    break;
  }

  case IR::OP_SHR: {
    // Logical shift right (unsigned)
    u16 dest_reg = ir->destinations[ir_idx];
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 shift_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, base_reg);
    CoreType base_type =
        static_cast<CoreType>(ir->registerTypes[base_reg & 0x3FFF]);
    bool useUint = (base_type == CoreType::UINT);
    u32 base = GetSpirvIdForBitwise(base_reg, useUint);
    u32 shift = GetSpirvIdForBitwise(shift_reg, useUint);
    Emit(spv::OpShiftRightLogical, result_type, dest, base, shift);
    break;
  }

  case IR::OP_ASR: {
    // Arithmetic shift right (signed)
    u16 dest_reg = ir->destinations[ir_idx];
    u32 base = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 shift = GetSpirvId(ir->GetOperand(ir_idx, 1));
    u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
    Emit(spv::OpShiftRightArithmetic, result_type, dest, base, shift);
    break;
  }

  case IR::OP_POPCNT: {
    // Bit count (popcount)
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    Emit(spv::OpBitCount, result_type, dest, operand);
    break;
  }

  case IR::OP_REVERSE_BITS: {
    // Bit reverse
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(op_reg);
    u32 result_type = GetResultType(dest_reg, op_reg);
    Emit(spv::OpBitReverse, result_type, dest, operand);
    break;
  }

  // ========== Float Comparison ==========
  case IR::OP_FEQ:
  case IR::OP_FNE:
  case IR::OP_FLT:
  case IR::OP_FLE:
  case IR::OP_FGT:
  case IR::OP_FGE: {
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Determine operand types to check for vector comparison
    // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are
    // scalars
    bool op1_is_scalar = (op1_reg & 0xE000) != 0;
    bool op2_is_scalar = (op2_reg & 0xE000) != 0;
    CoreType op1_type = CoreType::FLOAT;
    CoreType op2_type = CoreType::FLOAT;

    if (ir->registerTypes) {
      if (!op1_is_scalar && op1_reg < ir->registerCount) {
        op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        op1_is_scalar =
            (op1_type == CoreType::FLOAT || op1_type == CoreType::INT ||
             op1_type == CoreType::UINT);
      }
      if (!op2_is_scalar && op2_reg < ir->registerCount) {
        op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        op2_is_scalar =
            (op2_type == CoreType::FLOAT || op2_type == CoreType::INT ||
             op2_type == CoreType::UINT);
      }
    }

    // Determine result type and number of components
    u32 result_type;
    u32 numComponents = 1;
    CoreType vectorType = CoreType::FLOAT;

    if (!op1_is_scalar) {
      vectorType = op1_type;
    } else if (!op2_is_scalar) {
      vectorType = op2_type;
    }

    // Determine number of components based on vector type
    if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 ||
        vectorType == CoreType::UINT2) {
      numComponents = 2;
      result_type = GetTypeId(CoreType::BOOL2);
    } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 ||
               vectorType == CoreType::UINT3) {
      numComponents = 3;
      result_type = GetTypeId(CoreType::BOOL3);
    } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 ||
               vectorType == CoreType::UINT4) {
      numComponents = 4;
      result_type = GetTypeId(CoreType::BOOL4);
    } else {
      // Scalar comparison
      result_type = GetTypeId(CoreType::BOOL);
    }

    // If one operand is scalar and other is vector, splat the scalar
    if (numComponents > 1) {
      u32 vec_type = GetTypeId(vectorType);

      if (op1_is_scalar && !op2_is_scalar) {
        // Splat op1 to vector
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op1;
        }
        op1 = splatted;
      } else if (op2_is_scalar && !op1_is_scalar) {
        // Splat op2 to vector
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op2;
        }
        op2 = splatted;
      }
    }

    // Map IR opcode to SPIR-V opcode
    spv::Op cmp_op;
    switch (op) {
    case IR::OP_FEQ:
      cmp_op = spv::OpFOrdEqual;
      break;
    case IR::OP_FNE:
      cmp_op = spv::OpFOrdNotEqual;
      break;
    case IR::OP_FLT:
      cmp_op = spv::OpFOrdLessThan;
      break;
    case IR::OP_FLE:
      cmp_op = spv::OpFOrdLessThanEqual;
      break;
    case IR::OP_FGT:
      cmp_op = spv::OpFOrdGreaterThan;
      break;
    case IR::OP_FGE:
      cmp_op = spv::OpFOrdGreaterThanEqual;
      break;
    default:
      cmp_op = spv::OpNop;
      break;
    }

    Emit(cmp_op, result_type, dest, op1, op2);
    break;
  }

  // ========== Integer Comparison (Signed) ==========
  case IR::OP_IEQ:
  case IR::OP_INE:
  case IR::OP_ILT:
  case IR::OP_ILE:
  case IR::OP_IGT:
  case IR::OP_IGE: {
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Detect operand type (scalar vs vector, for sizing the bvec result).
    CoreType op1_type = CoreType::INT;
    if (ir->registerTypes && (op1_reg & 0xE000) == 0 &&
        op1_reg < ir->registerCount) {
      op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
    } else if ((op1_reg & 0xC000) == 0xC000) {
      op1_type = CoreType::BOOL;
    }
    CoreType op2_type = CoreType::INT;
    if (ir->registerTypes && (op2_reg & 0xE000) == 0 &&
        op2_reg < ir->registerCount) {
      op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
    } else if ((op2_reg & 0xC000) == 0xC000) {
      op2_type = CoreType::BOOL;
    }

    // Pick the boolean result type from whichever operand is a vector.
    CoreType bvecType = CoreType::BOOL;
    auto vecWidth = [](CoreType t) -> u32 {
      if (t == CoreType::INT2 || t == CoreType::UINT2 ||
          t == CoreType::BOOL2) return 2;
      if (t == CoreType::INT3 || t == CoreType::UINT3 ||
          t == CoreType::BOOL3) return 3;
      if (t == CoreType::INT4 || t == CoreType::UINT4 ||
          t == CoreType::BOOL4) return 4;
      return 1;
    };
    u32 resultWidth = vecWidth(op1_type);
    if (resultWidth == 1) resultWidth = vecWidth(op2_type);
    if (resultWidth == 2) { bvecType = CoreType::BOOL2; }
    else if (resultWidth == 3) { bvecType = CoreType::BOOL3; }
    else if (resultWidth == 4) { bvecType = CoreType::BOOL4; }
    u32 bool_type = GetTypeId(bvecType);

    spv::Op cmp_op;
    bool op1IsBool =
        (op1_type == CoreType::BOOL || op1_type == CoreType::BOOL2 ||
         op1_type == CoreType::BOOL3 || op1_type == CoreType::BOOL4);
    if (op1IsBool) {
      // Use logical operations for booleans (scalar or vector).
      switch (op) {
      case IR::OP_IEQ:
        cmp_op = spv::OpLogicalEqual;
        break;
      case IR::OP_INE:
        cmp_op = spv::OpLogicalNotEqual;
        break;
      default:
        // Less than/greater than don't make sense for booleans
        // Fall back to integer comparison (will fail validation if wrong)
        cmp_op = spv::OpIEqual;
        break;
      }
    } else {
      switch (op) {
      case IR::OP_IEQ:
        cmp_op = spv::OpIEqual;
        break;
      case IR::OP_INE:
        cmp_op = spv::OpINotEqual;
        break;
      case IR::OP_ILT:
        cmp_op = spv::OpSLessThan;
        break;
      case IR::OP_ILE:
        cmp_op = spv::OpSLessThanEqual;
        break;
      case IR::OP_IGT:
        cmp_op = spv::OpSGreaterThan;
        break;
      case IR::OP_IGE:
        cmp_op = spv::OpSGreaterThanEqual;
        break;
      default:
        cmp_op = spv::OpNop;
        break;
      }
    }

    Emit(cmp_op, bool_type, dest, op1, op2);
    break;
  }

  // ========== Integer Comparison (Unsigned) ==========
  case IR::OP_ULT:
  case IR::OP_ULE:
  case IR::OP_UGT:
  case IR::OP_UGE: {
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);

    // Pick result vector width — uvec comparisons return a bvec of
    // matching size. Without this the result type fell back to bool
    // scalar and SPIR-V rejected the emit.
    CoreType op1_type = CoreType::UINT;
    if (ir->registerTypes && (op1_reg & 0xE000) == 0 &&
        op1_reg < ir->registerCount) {
      op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
    }
    CoreType op2_type = CoreType::UINT;
    if (ir->registerTypes && (op2_reg & 0xE000) == 0 &&
        op2_reg < ir->registerCount) {
      op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
    }
    auto vecWidthU = [](CoreType t) -> u32 {
      if (t == CoreType::UINT2 || t == CoreType::INT2 ||
          t == CoreType::BOOL2) return 2;
      if (t == CoreType::UINT3 || t == CoreType::INT3 ||
          t == CoreType::BOOL3) return 3;
      if (t == CoreType::UINT4 || t == CoreType::INT4 ||
          t == CoreType::BOOL4) return 4;
      return 1;
    };
    u32 wu = vecWidthU(op1_type);
    if (wu == 1) wu = vecWidthU(op2_type);
    CoreType bvecTy = CoreType::BOOL;
    if (wu == 2) bvecTy = CoreType::BOOL2;
    else if (wu == 3) bvecTy = CoreType::BOOL3;
    else if (wu == 4) bvecTy = CoreType::BOOL4;
    u32 bool_type = GetTypeId(bvecTy);

    spv::Op cmp_op;
    switch (op) {
    case IR::OP_ULT:
      cmp_op = spv::OpULessThan;
      break;
    case IR::OP_ULE:
      cmp_op = spv::OpULessThanEqual;
      break;
    case IR::OP_UGT:
      cmp_op = spv::OpUGreaterThan;
      break;
    case IR::OP_UGE:
      cmp_op = spv::OpUGreaterThanEqual;
      break;
    default:
      cmp_op = spv::OpNop;
      break;
    }

    Emit(cmp_op, bool_type, dest, op1, op2);
    break;
  }

  // ========== Select (Ternary) ==========
  case IR::OP_SELECT: {
    // OpSelect: result = condition ? true_val : false_val
    // BWSL select(a, b, cond) = if cond then b else a
    // IR convention: SELECT(false_val, true_val, condition)
    // SPIR-V OpSelect: OpSelect result_type result condition true_val false_val
    u16 false_val_reg = ir->GetOperand(ir_idx, 0); // First arg is false value
    u16 true_val_reg = ir->GetOperand(ir_idx, 1);  // Second arg is true value
    u16 cond_reg = ir->GetOperand(ir_idx, 2);
    u32 condition = GetSpirvId(cond_reg); // Third arg is condition
    u32 true_val = GetSpirvId(true_val_reg);
    u32 false_val = GetSpirvId(false_val_reg);

    // Result type must match the value types (NOT the condition type)
    // Get type from true_val operand, not from destination (which may be
    // incorrectly typed as bool)
    CoreType valType = CoreType::FLOAT;
    if (ir->registerTypes && true_val_reg < ir->registerCount) {
      valType = static_cast<CoreType>(ir->registerTypes[true_val_reg]);
    } else if ((true_val_reg & 0xC000) == 0xC000) {
      valType = CoreType::BOOL; // Bool constant (0xC000 prefix)
    } else if (true_val_reg & 0x8000) {
      valType = CoreType::FLOAT; // Float constant
    } else if (true_val_reg & 0x4000) {
      valType = CoreType::INT; // Int constant
    } else if (true_val_reg & 0x2000) {
      valType = CoreType::UINT; // Uint constant
    }
    u32 result_type = GetTypeId(valType);

    // Check if result is a vector - if so, we need to splat scalar bool
    // condition to bool vector
    CoreType destType = valType;

    u32 numComponents = 0;
    switch (destType) {
    case CoreType::FLOAT2:
    case CoreType::INT2:
    case CoreType::UINT2:
      numComponents = 2;
      break;
    case CoreType::FLOAT3:
    case CoreType::INT3:
    case CoreType::UINT3:
      numComponents = 3;
      break;
    case CoreType::FLOAT4:
    case CoreType::INT4:
    case CoreType::UINT4:
      numComponents = 4;
      break;
    default:
      numComponents = 0;
      break; // Scalar result, no splatting needed
    }

    CoreType condType = GetOperandType(cond_reg);
    u32 condComponents = CoreTypeScalarComponentCount(condType);
    bool condIsBool = (condType == CoreType::BOOL || condType == CoreType::BOOL2 ||
                       condType == CoreType::BOOL3 || condType == CoreType::BOOL4);

    if (!condIsBool) {
      // Convert numeric condition to bool (or bool vector) via != 0
      CoreType condScalarType = GetScalarComponentType(condType);
      CoreType boolType = CoreType::BOOL;
      if (condComponents == 2) {
        boolType = CoreType::BOOL2;
      } else if (condComponents == 3) {
        boolType = CoreType::BOOL3;
      } else if (condComponents == 4) {
        boolType = CoreType::BOOL4;
      }

      u32 bool_type = GetTypeId(boolType);
      u32 zero = 0;
      if (condComponents == 1) {
        if (condScalarType == CoreType::FLOAT) {
          zero = GetFloatConstantId(0.0f);
        } else if (condScalarType == CoreType::UINT) {
          zero = GetIntConstantId(0, true);
        } else {
          zero = GetIntConstantId(0, false);
        }
      } else {
        u32 scalar_zero = 0;
        if (condScalarType == CoreType::FLOAT) {
          scalar_zero = GetFloatConstantId(0.0f);
        } else if (condScalarType == CoreType::UINT) {
          scalar_zero = GetIntConstantId(0, true);
        } else {
          scalar_zero = GetIntConstantId(0, false);
        }
        u32 vec_type = GetTypeId(condType);
        u32 zero_constituents[4] = {scalar_zero, scalar_zero, scalar_zero,
                                    scalar_zero};
        zero = GetCompositeConstantId(vec_type, zero_constituents,
                                      condComponents);
      }

      u32 bool_result = AllocateId();
      if (condScalarType == CoreType::FLOAT) {
        Emit(spv::OpFOrdNotEqual, bool_type, bool_result, condition, zero);
      } else {
        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
      }
      condition = bool_result;
      condType = boolType;
      condIsBool = true;
    }

    auto ExtractBoolComponent = [&](u32 composite_id, u32 index) -> u32 {
      u32 extracted = AllocateId();
      u32 bool_type = GetTypeId(CoreType::BOOL);
      if (currentFunctionSize + 5 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (5 << 16) | spv::OpCompositeExtract;
      currentFunction[currentFunctionSize++] = bool_type;
      currentFunction[currentFunctionSize++] = extracted;
      currentFunction[currentFunctionSize++] = composite_id;
      currentFunction[currentFunctionSize++] = index;
      return extracted;
    };

    if (numComponents == 0 && condComponents > 1) {
      // Scalar select expects a scalar condition; use the first lane.
      condition = ExtractBoolComponent(condition, 0);
      condComponents = 1;
    }

    if (numComponents > 0) {
      if (condComponents > 1 && condComponents != numComponents) {
        // Mismatched vector condition - fall back to the first component.
        condition = ExtractBoolComponent(condition, 0);
        condComponents = 1;
      }

      if (condComponents == 1) {
        // Splat scalar bool to bool vector
        u32 bool_vec_type =
            GetTypeId(numComponents == 2   ? CoreType::BOOL2
                      : numComponents == 3 ? CoreType::BOOL3
                                           : CoreType::BOOL4);
        u32 splatted_cond = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = bool_vec_type;
        currentFunction[currentFunctionSize++] = splatted_cond;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = condition;
        }
        condition = splatted_cond;
      }
    }

    // OpSelect: result_type result condition object1(true) object2(false)
    Emit(spv::OpSelect, result_type, dest, condition, true_val, false_val);
    break;
  }

  // ========== Type Conversion Operations ==========
  case IR::OP_I2F: {
    // Signed int to float: OpConvertSToF
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::FLOAT);
    Emit(spv::OpConvertSToF, result_type, dest, operand);
    break;
  }

  case IR::OP_U2F: {
    // Unsigned int to float: OpConvertUToF
    u16 src_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(src_reg);
    u32 result_type = GetTypeId(CoreType::FLOAT);

    // Check if source is a storage buffer pointer - if so, load from it first
    if (src_reg < idCapacity && storagePtrStorageClass[src_reg] != 0) {
      u32 uint_type = GetTypeId(CoreType::UINT);
      u32 loaded_id = AllocateId();
      Emit(spv::OpLoad, uint_type, loaded_id, operand);
      operand = loaded_id;
    }

    // Validate source type - must be uint or int (we'll convert to float either
    // way)
    CoreType srcType = CoreType::UINT;
    if (ir->registerTypes && src_reg < ir->registerCount) {
      srcType = static_cast<CoreType>(ir->registerTypes[src_reg]);
    }
    if (srcType == CoreType::UINT || srcType == CoreType::UINT2 ||
        srcType == CoreType::UINT3 || srcType == CoreType::UINT4) {
      Emit(spv::OpConvertUToF, result_type, dest, operand);
    } else if (srcType == CoreType::INT || srcType == CoreType::INT2 ||
               srcType == CoreType::INT3 || srcType == CoreType::INT4) {
      // Source is signed int, use OpConvertSToF instead
      Emit(spv::OpConvertSToF, result_type, dest, operand);
    } else if (srcType == CoreType::FLOAT || srcType == CoreType::FLOAT2 ||
               srcType == CoreType::FLOAT3 || srcType == CoreType::FLOAT4) {
      // Source is already float, just copy
      Emit(spv::OpCopyObject, result_type, dest, operand);
    } else {
      // Unknown/invalid type - attempt conversion anyway
      Emit(spv::OpConvertUToF, result_type, dest, operand);
    }
    break;
  }

  case IR::OP_F2I: {
    // Float to signed int: OpConvertFToS
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::INT);
    Emit(spv::OpConvertFToS, result_type, dest, operand);
    break;
  }

  case IR::OP_F2U: {
    // Float to unsigned int: OpConvertFToU
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 result_type = GetTypeId(CoreType::UINT);
    Emit(spv::OpConvertFToU, result_type, dest, operand);
    break;
  }

  case IR::OP_I2U:
  case IR::OP_U2I: {
    // Int/uint conversion is just bitcast (same bit representation)
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    CoreType resultType = (op == IR::OP_I2U) ? CoreType::UINT : CoreType::INT;
    u32 result_type = GetTypeId(resultType);
    Emit(spv::OpBitcast, result_type, dest, operand);
    break;
  }

  case IR::OP_BITCAST: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 operand_reg = ir->GetOperand(ir_idx, 0);
    u32 operand = GetSpirvId(operand_reg);
    u32 result_type = GetResultType(dest_reg, operand_reg);
    Emit(spv::OpBitcast, result_type, dest, operand);
    break;
  }

  // ========== Extended Instructions (GLSL.std.450) ==========
  // Single-operand functions (result type matches input type)
  case IR::OP_SQRT:
  case IR::OP_RSQRT:
  case IR::OP_EXP:
  case IR::OP_EXP2:
  case IR::OP_LOG:
  case IR::OP_LOG2:
  case IR::OP_SIN:
  case IR::OP_COS:
  case IR::OP_TAN:
  case IR::OP_ASIN:
  case IR::OP_ACOS:
  case IR::OP_ATAN:
  case IR::OP_SINH:
  case IR::OP_COSH:
  case IR::OP_TANH:
  case IR::OP_FLOOR:
  case IR::OP_CEIL:
  case IR::OP_ROUND:
  case IR::OP_TRUNC:
  case IR::OP_FRACT:
  case IR::OP_FABS:
  case IR::OP_IABS: // Integer abs (SAbs in GLSL.std.450)
  case IR::OP_SIGN:
  case IR::OP_NORMALIZE:
  case IR::OP_CLZ:
  case IR::OP_CTZ:
  case IR::OP_DEGREES:
  case IR::OP_RADIANS:
  case IR::OP_PACK_UNORM2X16:
  case IR::OP_UNPACK_UNORM2X16:
  case IR::OP_PACK_UNORM4X8:
  case IR::OP_UNPACK_UNORM4X8:
  case IR::OP_PACK_SNORM2X16:
  case IR::OP_UNPACK_SNORM2X16:
  case IR::OP_PACK_SNORM4X8:
  case IR::OP_UNPACK_SNORM4X8:
  case IR::OP_PACK_HALF2X16:
  case IR::OP_UNPACK_HALF2X16:
  case IR::OP_MODF_STRUCT:
  case IR::OP_FREXP_STRUCT: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 operand_reg = ir->GetOperand(ir_idx, 0);
    u32 result_type = GetResultType(dest_reg, operand_reg);
    u32 operand = GetSpirvId(operand_reg);
    u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

    // sign() is polymorphic: FSign for float, SSign for signed int.
    if (op == IR::OP_SIGN && ir->registerTypes &&
        operand_reg < ir->registerCount) {
      CoreType t = static_cast<CoreType>(ir->registerTypes[operand_reg]);
      if (t == CoreType::INT || t == CoreType::INT2 ||
          t == CoreType::INT3 || t == CoreType::INT4) {
        glsl_op = GLSLstd450SSign;
      }
    }

    // OpExtInst: result_type, result_id, set_id, instruction, operands...
    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = operand;
    break;
  }

  case IR::OP_BITFIELD_EXTRACT: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 offset_reg = ir->GetOperand(ir_idx, 1);
    u16 count_reg = ir->GetOperand(ir_idx, 2);
    u32 result_type = GetResultType(dest_reg, base_reg);
    u32 base_id = GetSpirvId(base_reg);
    u32 offset_id = GetSpirvId(offset_reg);
    u32 count_id = GetSpirvId(count_reg);
    CoreType base_type = GetOperandType(base_reg);
    spv::Op extract_op =
        (base_type == CoreType::UINT) ? spv::OpBitFieldUExtract
                                      : spv::OpBitFieldSExtract;
    Emit(extract_op, result_type, dest, base_id, offset_id, count_id);
    break;
  }

  case IR::OP_BITFIELD_INSERT: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 insert_reg = ir->GetOperand(ir_idx, 1);
    u16 offset_reg = ir->GetOperand(ir_idx, 2);
    u16 count_reg = ir->GetOperand(ir_idx, 3);
    u32 result_type = GetResultType(dest_reg, base_reg);
    u32 base_id = GetSpirvId(base_reg);
    u32 insert_id = GetSpirvId(insert_reg);
    u32 offset_id = GetSpirvId(offset_reg);
    u32 count_id = GetSpirvId(count_reg);
    Emit(spv::OpBitFieldInsert, result_type, dest, base_id, insert_id,
         offset_id, count_id);
    break;
  }

  // Saturate: clamp(x, 0.0, 1.0) - single operand in IR, emits FClamp with
  // implicit 0,1
  case IR::OP_SATURATE: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op_reg = ir->GetOperand(ir_idx, 0);
    u32 result_type = GetResultType(dest_reg, op_reg);
    u32 operand = GetSpirvId(op_reg);
    u32 glsl_op = GLSLstd450FClamp;

    // Get scalar float constants
    u32 zero_scalar = GetFloatConstantId(0.0f);
    u32 one_scalar = GetFloatConstantId(1.0f);

    // For vector types, construct vector constants matching the operand type
    u32 zero_const = zero_scalar;
    u32 one_const = one_scalar;

    // Check if operand is a vector type
    CoreType opType = CoreType::FLOAT;
    if (ir->registerTypes && op_reg < ir->registerCount) {
      opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
    }

    u32 numComponents = 1;
    if (opType == CoreType::FLOAT2)
      numComponents = 2;
    else if (opType == CoreType::FLOAT3)
      numComponents = 3;
    else if (opType == CoreType::FLOAT4)
      numComponents = 4;

    if (numComponents > 1) {
      // Create vector constants by splatting scalar values
      u32 vec_type = GetTypeId(opType);

      // Create vec(0, 0, ...) - splat zero_scalar for all components
      u32 zero_constituents[4] = {zero_scalar, zero_scalar, zero_scalar,
                                  zero_scalar};
      zero_const =
          GetCompositeConstantId(vec_type, zero_constituents, numComponents);

      // Create vec(1, 1, ...) - splat one_scalar for all components
      u32 one_constituents[4] = {one_scalar, one_scalar, one_scalar,
                                 one_scalar};
      one_const =
          GetCompositeConstantId(vec_type, one_constituents, numComponents);
    }

    // OpExtInst: result_type, result_id, set_id, instruction, x, minVal, maxVal
    if (currentFunctionSize + 8 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = operand;
    currentFunction[currentFunctionSize++] = zero_const;
    currentFunction[currentFunctionSize++] = one_const;
    break;
  }

  // Single-operand functions that return scalar float (length, distance with 1
  // arg)
  case IR::OP_LENGTH: {
    u32 result_type =
        GetTypeId(CoreType::FLOAT); // Length always returns scalar
    u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = operand;
    break;
  }

  // Dot product - core SPIR-V op, returns scalar float
  case IR::OP_DOT: {
    u32 result_type = GetTypeId(CoreType::FLOAT); // Dot always returns scalar
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));

    // OpDot: result_type, result_id, vector1, vector2
    Emit(spv::OpDot, result_type, dest, op1, op2);
    break;
  }

  // Two-operand extended functions (result type matches input type)
  case IR::OP_POW:
  case IR::OP_ATAN2:
  case IR::OP_FMIN:
  case IR::OP_FMAX:
  case IR::OP_IMIN: // Integer min (SMin in GLSL.std.450)
  case IR::OP_IMAX: // Integer max (SMax in GLSL.std.450)
  case IR::OP_UMIN: // Unsigned min (UMin in GLSL.std.450)
  case IR::OP_UMAX: // Unsigned max (UMax in GLSL.std.450)
  case IR::OP_LDEXP:
  case IR::OP_STEP:
  case IR::OP_REFLECT:
  case IR::OP_CROSS: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u32 result_type = GetResultType(dest_reg, op1_reg);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);
    u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

    // For GLSL.std.450 functions like Pow, both operands must match result type
    // Handle scalar-vector mismatches by splatting scalar to vector
    // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are
    // scalars
    bool op1_is_scalar = (op1_reg & 0xE000) != 0;
    bool op2_is_scalar = (op2_reg & 0xE000) != 0;
    CoreType op1_type = CoreType::FLOAT;
    CoreType op2_type = CoreType::FLOAT;

    if (ir->registerTypes) {
      if (!op1_is_scalar && op1_reg < ir->registerCount) {
        op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        op1_is_scalar =
            (op1_type == CoreType::FLOAT || op1_type == CoreType::INT ||
             op1_type == CoreType::UINT);
      }
      if (!op2_is_scalar && op2_reg < ir->registerCount) {
        op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        op2_is_scalar =
            (op2_type == CoreType::FLOAT || op2_type == CoreType::INT ||
             op2_type == CoreType::UINT);
      }
    }

    // Determine vector type and component count
    // Skip VOID and INVALID types - only use valid vector types
    CoreType vectorType = CoreType::FLOAT;
    u32 numComponents = 1;
    if (!op1_is_scalar && op1_type != CoreType::VOID &&
        op1_type != CoreType::INVALID) {
      vectorType = op1_type;
    } else if (!op2_is_scalar && op2_type != CoreType::VOID &&
               op2_type != CoreType::INVALID) {
      vectorType = op2_type;
    }

    if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 ||
        vectorType == CoreType::UINT2) {
      numComponents = 2;
    } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 ||
               vectorType == CoreType::UINT3) {
      numComponents = 3;
    } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 ||
               vectorType == CoreType::UINT4) {
      numComponents = 4;
    }

    // Splat scalar operands to match vector type
    if (numComponents > 1) {
      u32 vec_type = GetTypeId(vectorType);
      // Update result_type to use the correct vector type
      result_type = vec_type;
      if (op1_is_scalar && !op2_is_scalar) {
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op1;
        }
        op1 = splatted;
      } else if (op2_is_scalar && !op1_is_scalar) {
        u32 splatted = AllocateId();
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op2;
        }
        op2 = splatted;
      }
    }

    if (currentFunctionSize + 7 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = op1;
    currentFunction[currentFunctionSize++] = op2;
    break;
  }

  // Distance returns scalar float
  case IR::OP_DISTANCE: {
    u32 result_type = GetTypeId(CoreType::FLOAT);
    u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
    u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

    if (currentFunctionSize + 7 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = op1;
    currentFunction[currentFunctionSize++] = op2;
    break;
  }

  // Lerp (FMix) - needs special handling for scalar interpolant with vector
  // operands
  case IR::OP_LERP: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u16 op3_reg = ir->GetOperand(ir_idx, 2);
    u32 result_type = GetResultType(dest_reg, op1_reg);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);
    u32 op3 = GetSpirvId(op3_reg);
    u32 glsl_op = GLSLstd450FMix;

    // Check if op3 (interpolant) is scalar while result is vector
    // SPIR-V FMix requires all operands to match result type
    // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are
    // scalars
    bool op3_is_scalar = (op3_reg & 0xE000) != 0;
    CoreType op1_type = CoreType::FLOAT;
    CoreType op3_type = CoreType::FLOAT;

    if (ir->registerTypes) {
      if (op1_reg < ir->registerCount) {
        op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
      }
      if (!op3_is_scalar && op3_reg < ir->registerCount) {
        op3_type = static_cast<CoreType>(ir->registerTypes[op3_reg]);
        op3_is_scalar =
            (op3_type == CoreType::FLOAT || op3_type == CoreType::INT ||
             op3_type == CoreType::UINT);
      }
    }

    // Determine if we need to splat op3 to match vector type
    u32 numComponents = 1;
    if (op1_type == CoreType::FLOAT2 || op1_type == CoreType::INT2 ||
        op1_type == CoreType::UINT2) {
      numComponents = 2;
    } else if (op1_type == CoreType::FLOAT3 || op1_type == CoreType::INT3 ||
               op1_type == CoreType::UINT3) {
      numComponents = 3;
    } else if (op1_type == CoreType::FLOAT4 || op1_type == CoreType::INT4 ||
               op1_type == CoreType::UINT4) {
      numComponents = 4;
    }

    // Splat scalar interpolant to vector if needed
    if (numComponents > 1 && op3_is_scalar) {
      u32 vec_type = GetTypeId(op1_type);
      result_type = vec_type;

      if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      u32 splatted = AllocateId();
      currentFunction[currentFunctionSize++] =
          ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
      currentFunction[currentFunctionSize++] = vec_type;
      currentFunction[currentFunctionSize++] = splatted;
      for (u32 i = 0; i < numComponents; i++) {
        currentFunction[currentFunctionSize++] = op3;
      }
      op3 = splatted;
    }

    if (currentFunctionSize + 8 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = op1;
    currentFunction[currentFunctionSize++] = op2;
    currentFunction[currentFunctionSize++] = op3;
    break;
  }

  // Three-operand extended functions
  case IR::OP_FCLAMP:
  case IR::OP_ICLAMP: // Integer clamp (SClamp in GLSL.std.450)
  case IR::OP_UCLAMP: // Unsigned clamp (UClamp in GLSL.std.450)
  case IR::OP_SMOOTHSTEP:
  case IR::OP_FMA:
  case IR::OP_FACEFORWARD:
  case IR::OP_REFRACT: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 op1_reg = ir->GetOperand(ir_idx, 0);
    u16 op2_reg = ir->GetOperand(ir_idx, 1);
    u16 op3_reg = ir->GetOperand(ir_idx, 2);
    u32 result_type = GetResultType(dest_reg, op1_reg);
    u32 op1 = GetSpirvId(op1_reg);
    u32 op2 = GetSpirvId(op2_reg);
    u32 op3 = GetSpirvId(op3_reg);
    u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

    // For GLSL.std.450 ops (except Refract), operands must match result type.
    // Splat scalar operands to vector when needed.
    bool op1_is_scalar = (op1_reg & 0xE000) != 0;
    bool op2_is_scalar = (op2_reg & 0xE000) != 0;
    bool op3_is_scalar = (op3_reg & 0xE000) != 0;
    CoreType op1_type = CoreType::FLOAT;
    CoreType op2_type = CoreType::FLOAT;
    CoreType op3_type = CoreType::FLOAT;

    if (ir->registerTypes) {
      if (!op1_is_scalar && op1_reg < ir->registerCount) {
        op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        op1_is_scalar =
            (op1_type == CoreType::FLOAT || op1_type == CoreType::INT ||
             op1_type == CoreType::UINT);
      }
      if (!op2_is_scalar && op2_reg < ir->registerCount) {
        op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
        op2_is_scalar =
            (op2_type == CoreType::FLOAT || op2_type == CoreType::INT ||
             op2_type == CoreType::UINT);
      }
      if (!op3_is_scalar && op3_reg < ir->registerCount) {
        op3_type = static_cast<CoreType>(ir->registerTypes[op3_reg]);
        op3_is_scalar =
            (op3_type == CoreType::FLOAT || op3_type == CoreType::INT ||
             op3_type == CoreType::UINT);
      }
    }

    CoreType vectorType = CoreType::FLOAT;
    if (!op1_is_scalar && op1_type != CoreType::VOID &&
        op1_type != CoreType::INVALID) {
      vectorType = op1_type;
    } else if (!op2_is_scalar && op2_type != CoreType::VOID &&
               op2_type != CoreType::INVALID) {
      vectorType = op2_type;
    } else if (!op3_is_scalar && op3_type != CoreType::VOID &&
               op3_type != CoreType::INVALID) {
      vectorType = op3_type;
    }

    u32 numComponents = 1;
    if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 ||
        vectorType == CoreType::UINT2) {
      numComponents = 2;
    } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 ||
               vectorType == CoreType::UINT3) {
      numComponents = 3;
    } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 ||
               vectorType == CoreType::UINT4) {
      numComponents = 4;
    }

    if (numComponents > 1 && op != IR::OP_REFRACT) {
      u32 vec_type = GetTypeId(vectorType);
      result_type = vec_type;
      if (op1_is_scalar) {
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        u32 splatted = AllocateId();
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op1;
        }
        op1 = splatted;
      }
      if (op2_is_scalar) {
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        u32 splatted = AllocateId();
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op2;
        }
        op2 = splatted;
      }
      if (op3_is_scalar) {
        if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        u32 splatted = AllocateId();
        currentFunction[currentFunctionSize++] =
            ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
        currentFunction[currentFunctionSize++] = vec_type;
        currentFunction[currentFunctionSize++] = splatted;
        for (u32 i = 0; i < numComponents; i++) {
          currentFunction[currentFunctionSize++] = op3;
        }
        op3 = splatted;
      }
    }

    if (currentFunctionSize + 8 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = glslStd450Id;
    currentFunction[currentFunctionSize++] = glsl_op;
    currentFunction[currentFunctionSize++] = op1;
    currentFunction[currentFunctionSize++] = op2;
    currentFunction[currentFunctionSize++] = op3;
    break;
  }

  case IR::OP_LOAD_ATTR: {
    u8 attr_idx = (u8)ir->GetOperand(ir_idx, 0);

    // Get the attribute type
    CoreType attrType =
        static_cast<CoreType>(analysis.attributeTypes[attr_idx]);
    if (attrType == CoreType::VOID || attrType == CoreType::INVALID ||
        static_cast<u8>(attrType) == 0) {
      attrType = GetFallbackAttributeType(attr_idx);
    }
    u32 load_type_id = GetTypeId(attrType);

    if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers) {
      // Vertex pulling: load from storage buffer indexed by gl_VertexIndex
      // 1. Load vertex index
      u32 uint_type = GetTypeId(CoreType::UINT);
      u32 vertex_idx_id = AllocateId();
      Emit(spv::OpLoad, uint_type, vertex_idx_id, vertexIdVarId);

      // 2. AccessChain into buffer[0][vertex_idx]
      // Buffer struct is: struct { T[] data; }
      // Access path: buffer -> member 0 -> array[vertex_idx]
      u32 buffer_var_id = attributeBufferIds[attr_idx];
      if (buffer_var_id == 0)
        break; // Buffer not declared

      u32 zero_const = GetIntConstantId(0);
      u32 element_ptr_type =
          GetPointerTypeId(load_type_id, spv::StorageClassStorageBuffer);
      u32 element_ptr_id = AllocateId();

      // OpAccessChain: result_type, result, base, indices...
      // Indices: 0 (first struct member = the array), vertex_idx (array
      // element)
      if (currentFunctionSize + 6 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpAccessChain;
      currentFunction[currentFunctionSize++] = element_ptr_type;
      currentFunction[currentFunctionSize++] = element_ptr_id;
      currentFunction[currentFunctionSize++] = buffer_var_id;
      currentFunction[currentFunctionSize++] = zero_const;    // struct member 0
      currentFunction[currentFunctionSize++] = vertex_idx_id; // array index

      // 3. Load the value
      Emit(spv::OpLoad, load_type_id, dest, element_ptr_id);
    } else if (vertexPullingConfig.mode ==
               VertexInputMode::UnifiedWithOffsets) {
      // TODO: Unified buffer mode - load from single buffer with offset table
      // For now, fall through to interleaved handling
      // This would need: load offset from offset table, compute address, load
      // value
    } else {
      // Interleaved mode: load from input variable
      u32 ptr_id = 0;
      for (u32 i = 0; i < inputCount; i++) {
        if (inputLocations[i] == attr_idx) {
          ptr_id = inputIds[i];
          break;
        }
      }
      if (ptr_id != 0 && load_type_id != 0) {
        Emit(spv::OpLoad, load_type_id, dest, ptr_id);
      }
    }
    break;
  }

  case IR::OP_LOAD_INPUT: {
    u32 input_slot = ir->GetOperand(ir_idx, 0);

    // Handle built-in inputs (vertex_id, instance_id)
    if (input_slot == BuiltinInputSlot::VERTEX_ID) {
      u32 uint_type = GetTypeId(CoreType::UINT);
      if (vertexIdVarId != 0) {
        Emit(spv::OpLoad, uint_type, dest, vertexIdVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::INSTANCE_ID) {
      u32 uint_type = GetTypeId(CoreType::UINT);
      if (instanceIdVarId != 0) {
        Emit(spv::OpLoad, uint_type, dest, instanceIdVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::GLOBAL_INVOCATION_ID) {
      u32 uint3_type = GetTypeId(CoreType::UINT3);
      if (globalInvocationIdVarId != 0) {
        Emit(spv::OpLoad, uint3_type, dest, globalInvocationIdVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::LOCAL_INVOCATION_ID) {
      u32 uint3_type = GetTypeId(CoreType::UINT3);
      if (localInvocationIdVarId != 0) {
        Emit(spv::OpLoad, uint3_type, dest, localInvocationIdVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::WORKGROUP_ID) {
      u32 uint3_type = GetTypeId(CoreType::UINT3);
      if (workgroupIdVarId != 0) {
        Emit(spv::OpLoad, uint3_type, dest, workgroupIdVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::NUM_WORKGROUPS) {
      u32 uint3_type = GetTypeId(CoreType::UINT3);
      if (numWorkgroupsVarId != 0) {
        Emit(spv::OpLoad, uint3_type, dest, numWorkgroupsVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::LOCAL_INVOCATION_INDEX) {
      u32 uint_type = GetTypeId(CoreType::UINT);
      if (localInvocationIndexVarId != 0) {
        Emit(spv::OpLoad, uint_type, dest, localInvocationIndexVarId);
      }
      break;
    }
    if (input_slot == BuiltinInputSlot::FRAG_COORD) {
      u32 float4_type = GetTypeId(CoreType::FLOAT4);
      if (fragCoordVarId != 0) {
        Emit(spv::OpLoad, float4_type, dest, fragCoordVarId);
      }
      break;
    }

    // Fragment shader loading interpolated varying from vertex output
    // Get the input type - for varyings, default to float3
    CoreType inputType = CoreType::FLOAT3;
    if (ir->registerTypes && ir->destinations[ir_idx] < ir->registerCount) {
      CoreType regType =
          static_cast<CoreType>(ir->registerTypes[ir->destinations[ir_idx]]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        inputType = regType;
      }
    }
    u32 load_type_id = GetTypeId(inputType);

    // Find the input variable by slot/location
    u32 ptr_id = 0;
    for (u32 i = 0; i < inputCount; i++) {
      if (inputLocations[i] == input_slot) {
        ptr_id = inputIds[i];
        break;
      }
    }

    if (ptr_id != 0 && load_type_id != 0) {
      Emit(spv::OpLoad, load_type_id, dest, ptr_id);
    }
    break;
  }

  case IR::OP_LOAD_UNIFORM: {
    // Load from uniform buffer
    u32 binding = ir->GetOperand(ir_idx, 0);
    u16 dest_reg = ir->destinations[ir_idx];

    // Array-typed uniforms are not loaded as a whole. The destination
    // register becomes an alias for the UBO variable so that subsequent
    // OP_STORAGE_INDEX/OP_STORAGE_LOAD access-chain into it (member 0 is the
    // array; the leading zero index is added by OP_STORAGE_INDEX).
    if (ir->registerStorageInfo && dest_reg < ir->registerCount &&
        (ir->registerStorageInfo[dest_reg] &
         IR::IRProgram::STORAGE_IS_UNIFORM_ARRAY)) {
      u32 ubo_var_id = uniformBufferIds[binding];
      if (ubo_var_id != 0) {
        spirvIds[dest_reg] = ubo_var_id;
        if (dest_reg < idCapacity) {
          storagePtrStorageClass[dest_reg] =
              static_cast<u32>(spv::StorageClassUniform);
        }
      } else {
        u32 type_id = GetTypeId(CoreType::UINT);
        Emit(spv::OpUndef, type_id, dest);
      }
      break;
    }

    // Get the uniform type from register type info
    CoreType uniformType = CoreType::FLOAT4; // Default
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        uniformType = regType;
      }
    }
    // Get type ID - use GetStructTypeId for CUSTOM/ENUM types
    u32 load_type_id = 0;
    if ((uniformType == CoreType::CUSTOM || uniformType == CoreType::ENUM) &&
        ir->registerStructTypes && dest_reg < ir->registerCount) {
      u32 structHash = ir->registerStructTypes[dest_reg];
      if (structHash != 0) {
        load_type_id = GetStructTypeId(structHash);
      }
    }
    if (load_type_id == 0) {
      load_type_id = GetTypeId(uniformType);
    }

    // Get the uniform buffer variable
    u32 buffer_var_id = uniformBufferIds[binding];
    if (buffer_var_id != 0 && load_type_id != 0) {
      // Access chain to get pointer to the first (and only) member of the
      // struct
      u32 zero_const = GetIntConstantId(0);
      u32 member_ptr_type =
          GetPointerTypeId(load_type_id, spv::StorageClassUniform);
      u32 member_ptr_id = AllocateId();

      // OpAccessChain: result_type, result, base, index
      if (currentFunctionSize + 5 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpAccessChain;
      currentFunction[currentFunctionSize++] = member_ptr_type;
      currentFunction[currentFunctionSize++] = member_ptr_id;
      currentFunction[currentFunctionSize++] = buffer_var_id;
      currentFunction[currentFunctionSize++] = zero_const; // member index 0

      // Load the value
      Emit(spv::OpLoad, load_type_id, dest, member_ptr_id);
    }
    break;
  }

  case IR::OP_LOAD_OUTPUT: {
    u32 slot = ir->GetOperand(ir_idx, 0);
    u16 dest_reg = ir->destinations[ir_idx];
    u32 dest2 = GetSpirvId(dest_reg);

    CoreType outputType = CoreType::FLOAT4;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        outputType = regType;
      }
    }
    if (outputType == CoreType::INVALID || outputType == CoreType::VOID) {
      outputType = GetFallbackOutputType(slot);
    }
    u32 load_type_id = GetTypeId(outputType);

    u32 ptr_id = 0;
    for (u32 i = 0; i < outputCount; i++) {
      if (outputLocations[i] == slot ||
          (outputLocations[i] == 0xFF && slot == OutputSlot::POSITION)) {
        ptr_id = outputIds[i];
        break;
      }
    }
    if (ptr_id != 0 && load_type_id != 0) {
      Emit(spv::OpLoad, load_type_id, dest2, ptr_id);
    }
    break;
  }

  case IR::OP_STORE_OUTPUT: {
    // Note: IR lowering puts the value register in destinations, not operands
    // EmitInstruction(OP_STORE_OUTPUT, valueReg, slot) -> destinations =
    // valueReg
    u32 value = GetSpirvId(ir->destinations[ir_idx]);

    // Slot is now stored in operand[0] (set during IR lowering)
    // This enables dynamic vertex-to-fragment varying resolution
    u32 slot = ir->GetOperand(ir_idx, 0);

    // Find the output variable by looking through our declared outputs
    u32 ptr_id = 0;
    for (u32 i = 0; i < outputCount; i++) {
      if (outputLocations[i] == slot ||
          (outputLocations[i] == 0xFF && slot == OutputSlot::POSITION)) {
        ptr_id = outputIds[i];
        break;
      }
    }
    if (ptr_id != 0) {
      Emit(spv::OpStore, ptr_id, value);
    }
    break;
  }

  // ========== Vector Operations ==========
  case IR::OP_VEC_CONSTRUCT: {
    // Build a vector from components
    // IR now has 4 operand slots natively for float4 support
    // operands use 0xFFFF as sentinel for "unused"
    u16 dest_reg = ir->destinations[ir_idx];
    CoreType resultType = CoreType::FLOAT4;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      // Only use register type if it's a valid vector type, not CUSTOM or
      // INVALID
      if (regType != CoreType::CUSTOM && regType != CoreType::ENUM &&
          regType != CoreType::INVALID && regType != CoreType::VOID) {
        resultType = regType;
      }
    }

    u32 result_type_id = GetTypeId(resultType);

    // Get actual argument count from metadata
    u32 argCount = ir->metadata[ir_idx];
    if (argCount == 0 || argCount > 4)
      argCount = 4; // Fallback

    // Collect input operands (0xFFFF means unused)
    // Store both SPIR-V IDs and original register numbers for type checking
    u32 inputIds[4] = {0, 0, 0, 0};
    u16 inputRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    u32 inputCount = 0;

    // All 4 operands from operands array
    for (u32 c = 0; c < argCount; c++) {
      u16 op_reg = ir->GetOperand(ir_idx, c);
      if (op_reg == 0xFFFF)
        continue; // Sentinel for unused
      inputRegs[inputCount] = op_reg;
      u32 op_id = GetSpirvId(op_reg);
      // Check if this is a storage buffer pointer - if so, load from it
      if (op_reg < idCapacity && storagePtrStorageClass[op_reg] != 0) {
        // Determine the type to load based on register type
        CoreType loadType = CoreType::FLOAT;
        if (ir->registerTypes && op_reg < ir->registerCount) {
          loadType = static_cast<CoreType>(ir->registerTypes[op_reg]);
        }
        u32 load_type_id = GetTypeId(loadType);
        u32 loaded_id = AllocateId();
        Emit(spv::OpLoad, load_type_id, loaded_id, op_id);
        op_id = loaded_id;
      }
      inputIds[inputCount++] = op_id;
    }

    // OpCompositeConstruct can take mixed vectors and scalars
    // Handle scalar broadcast: float3(x) -> OpCompositeConstruct %v3float %x %x
    // %x
    if (inputCount == 0) {
      // No operands provided: emit a zero vector to ensure a defined ID
      u32 requiredComponents = 1;
      switch (resultType) {
      case CoreType::FLOAT2:
      case CoreType::INT2:
      case CoreType::UINT2:
      case CoreType::BOOL2:
        requiredComponents = 2;
        break;
      case CoreType::FLOAT3:
      case CoreType::INT3:
      case CoreType::UINT3:
      case CoreType::BOOL3:
        requiredComponents = 3;
        break;
      case CoreType::FLOAT4:
      case CoreType::INT4:
      case CoreType::UINT4:
      case CoreType::BOOL4:
        requiredComponents = 4;
        break;
      default:
        break;
      }

      CoreType desiredScalarType = GetScalarComponentType(resultType);
      u32 zeroId = 0;
      if (desiredScalarType == CoreType::FLOAT) {
        zeroId = GetFloatConstantId(0.0f);
      } else if (desiredScalarType == CoreType::INT) {
        zeroId = GetIntConstantId(0, false);
      } else if (desiredScalarType == CoreType::UINT) {
        zeroId = GetIntConstantId(0, true);
      } else if (desiredScalarType == CoreType::BOOL) {
        zeroId = GetBoolConstantId(false);
      }

      u32 wordCount = 3 + requiredComponents;
      if (currentFunctionSize + wordCount > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (wordCount << 16) | spv::OpCompositeConstruct;
      currentFunction[currentFunctionSize++] = result_type_id;
      currentFunction[currentFunctionSize++] = dest;
      for (u32 c = 0; c < requiredComponents; c++) {
        currentFunction[currentFunctionSize++] = zeroId;
      }
      break;
    }

    if (inputCount > 0) {
      // Determine required component count for target vector type
      u32 requiredComponents = 1;
      switch (resultType) {
      case CoreType::FLOAT2:
      case CoreType::INT2:
      case CoreType::UINT2:
      case CoreType::BOOL2:
        requiredComponents = 2;
        break;
      case CoreType::FLOAT3:
      case CoreType::INT3:
      case CoreType::UINT3:
      case CoreType::BOOL3:
        requiredComponents = 3;
        break;
      case CoreType::FLOAT4:
      case CoreType::INT4:
      case CoreType::UINT4:
      case CoreType::BOOL4:
        requiredComponents = 4;
        break;
      default:
        break;
      }

      CoreType desiredScalarType = GetScalarComponentType(resultType);
      auto convertScalar = [&](u32 value_id, CoreType srcType) -> u32 {
        if (srcType == desiredScalarType)
          return value_id;
        u32 dest_type_id = GetTypeId(desiredScalarType);
        u32 converted = AllocateId();

        if (srcType == CoreType::BOOL) {
          u32 zero_id = 0;
          u32 one_id = 0;
          if (desiredScalarType == CoreType::FLOAT) {
            zero_id = GetFloatConstantId(0.0f);
            one_id = GetFloatConstantId(1.0f);
          } else if (desiredScalarType == CoreType::INT) {
            zero_id = GetIntConstantId(0, false);
            one_id = GetIntConstantId(1, false);
          } else if (desiredScalarType == CoreType::UINT) {
            zero_id = GetIntConstantId(0, true);
            one_id = GetIntConstantId(1, true);
          }
          Emit(spv::OpSelect, dest_type_id, converted, value_id, one_id,
               zero_id);
          return converted;
        }

        if (srcType == CoreType::INT && desiredScalarType == CoreType::FLOAT) {
          Emit(spv::OpConvertSToF, dest_type_id, converted, value_id);
          return converted;
        }
        if (srcType == CoreType::UINT && desiredScalarType == CoreType::FLOAT) {
          Emit(spv::OpConvertUToF, dest_type_id, converted, value_id);
          return converted;
        }
        if (srcType == CoreType::FLOAT && desiredScalarType == CoreType::INT) {
          Emit(spv::OpConvertFToS, dest_type_id, converted, value_id);
          return converted;
        }
        if (srcType == CoreType::FLOAT && desiredScalarType == CoreType::UINT) {
          Emit(spv::OpConvertFToU, dest_type_id, converted, value_id);
          return converted;
        }
        if (srcType == CoreType::INT && desiredScalarType == CoreType::UINT) {
          Emit(spv::OpBitcast, dest_type_id, converted, value_id);
          return converted;
        }
        if (srcType == CoreType::UINT && desiredScalarType == CoreType::INT) {
          Emit(spv::OpBitcast, dest_type_id, converted, value_id);
          return converted;
        }

        Emit(spv::OpBitcast, dest_type_id, converted, value_id);
        return converted;
      };

      // If result is scalar (requiredComponents == 1), just copy the value
      // OpCompositeConstruct requires at least 2 constituents
      if (requiredComponents == 1 && inputCount == 1) {
        // Just assign the scalar directly via OpCopyObject
        Emit(spv::OpCopyObject, result_type_id, dest, inputIds[0]);
        break;
      }

      // Check input types and decompose vectors into scalars
      // This handles float3(uv, 2.0) where uv is float2
      // We only extract enough components to satisfy requiredComponents
      u32 scalarIds[8]; // Enough for 2 * vec4 = 8 components max
      u32 scalarCount = 0;

      for (u32 c = 0; c < inputCount && scalarCount < requiredComponents; c++) {
        u16 op_reg = inputRegs[c];
        if (op_reg == 0xFFFF)
          continue;

        // Check the type of this operand
        CoreType opType = CoreType::FLOAT;
        if ((op_reg & 0xC000) == 0xC000) {
          opType = CoreType::BOOL;
        } else if (op_reg & 0x8000) {
          opType = CoreType::FLOAT;
        } else if (op_reg & 0x4000) {
          opType = CoreType::INT;
        } else if (op_reg & 0x2000) {
          opType = CoreType::UINT;
        } else if (ir->registerTypes && op_reg < ir->registerCount) {
          opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
        }

        // Determine how many components this operand contributes
        u32 opComponents = 1;
        switch (opType) {
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
          opComponents = 2;
          break;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
          opComponents = 3;
          break;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
          opComponents = 4;
          break;
        default:
          break;
        }

        CoreType opScalarType = GetScalarComponentType(opType);
        // If type is CUSTOM or unknown, use it as-is without conversion
        // (CUSTOM types shouldn't be components of vectors)
        if (opType == CoreType::CUSTOM || opType == CoreType::ENUM ||
            opType == CoreType::INVALID || opType == CoreType::VOID) {
          scalarIds[scalarCount++] = inputIds[c];
          continue;
        }
        if (opComponents == 1) {
          // Scalar - use directly
          scalarIds[scalarCount++] = convertScalar(inputIds[c], opScalarType);
        } else {
          // Vector - extract each component (but only as many as we still need)
          u32 componentsToExtract =
              (opComponents < (requiredComponents - scalarCount))
                  ? opComponents
                  : (requiredComponents - scalarCount);
          u32 op_scalar_type_id = GetTypeId(opScalarType);
          for (u32 comp = 0; comp < componentsToExtract; comp++) {
            u32 extracted = AllocateId();
            // OpCompositeExtract: result_type result_id composite index...
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
              GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] =
                (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = op_scalar_type_id;
            currentFunction[currentFunctionSize++] = extracted;
            currentFunction[currentFunctionSize++] = inputIds[c];
            currentFunction[currentFunctionSize++] = comp;
            scalarIds[scalarCount++] = convertScalar(extracted, opScalarType);
          }
        }
      }

      // If we have fewer scalars than required components, broadcast/splat
      // This handles float3(0.0) -> OpCompositeConstruct %v3float %0 %0 %0
      u32 actualCount = scalarCount;
      if (scalarCount == 1 && requiredComponents > 1) {
        // Scalar broadcast - replicate the single input
        u32 scalarId = scalarIds[0];
        for (u32 c = 1; c < requiredComponents; c++) {
          scalarIds[c] = scalarId;
        }
        actualCount = requiredComponents;
      }

      if (actualCount < requiredComponents) {
        u32 zeroId = 0;
        if (desiredScalarType == CoreType::FLOAT) {
          zeroId = GetFloatConstantId(0.0f);
        } else if (desiredScalarType == CoreType::INT) {
          zeroId = GetIntConstantId(0, false);
        } else if (desiredScalarType == CoreType::UINT) {
          zeroId = GetIntConstantId(0, true);
        } else if (desiredScalarType == CoreType::BOOL) {
          zeroId = GetBoolConstantId(false);
        }

        while (actualCount < requiredComponents) {
          scalarIds[actualCount++] = zeroId;
        }
      }

      u32 wordCount = 3 + actualCount;
      if (currentFunctionSize + wordCount > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (wordCount << 16) | spv::OpCompositeConstruct;
      currentFunction[currentFunctionSize++] = result_type_id;
      currentFunction[currentFunctionSize++] = dest;
      for (u32 c = 0; c < actualCount; c++) {
        currentFunction[currentFunctionSize++] = scalarIds[c];
      }
    }
    break;
  }

  case IR::OP_MAT_CONSTRUCT: {
    // Build a matrix from column vectors (generated by IR lowering)
    // IR lowering now generates OP_VEC_CONSTRUCT for each column, then
    // OP_MAT_CONSTRUCT The operands are column vector registers, metadata
    // contains column count
    u16 dest_reg = ir->destinations[ir_idx];
    CoreType resultType = CoreType::MAT4;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      resultType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
    }

    u32 result_type_id = GetTypeId(resultType);

    // Determine number of columns from metadata
    u32 numColumns = ir->metadata[ir_idx];
    if (numColumns == 0) {
      numColumns = (resultType == CoreType::MAT2)   ? 2
                   : (resultType == CoreType::MAT3) ? 3
                                                    : 4;
    }

    // Collect column vector IDs
    u32 columnIds[4];
    for (u32 col = 0; col < numColumns; col++) {
      u16 op_reg = ir->GetOperand(ir_idx, col);
      columnIds[col] = GetSpirvId(op_reg);
    }

    // Build matrix from column vectors
    u32 matWordCount = 3 + numColumns;
    if (currentFunctionSize + matWordCount > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (matWordCount << 16) | spv::OpCompositeConstruct;
    currentFunction[currentFunctionSize++] = result_type_id;
    currentFunction[currentFunctionSize++] = dest;
    for (u32 col = 0; col < numColumns; col++) {
      currentFunction[currentFunctionSize++] = columnIds[col];
    }
    break;
  }

  case IR::OP_VEC_EXTRACT: {
    // Extract a single component from a vector
    // operand 0: source vector register
    // operand 1: component index (0-3 for x/y/z/w)
    u16 src_reg = ir->GetOperand(ir_idx, 0);
    u32 src_id = GetSpirvId(src_reg);
    u32 component_idx = ir->GetOperand(ir_idx, 1);

    // Determine the scalar component type from the source vector type
    CoreType scalarType = CoreType::FLOAT; // Default fallback
    if (ir->registerTypes && src_reg < ir->registerCount) {
      CoreType srcType = static_cast<CoreType>(ir->registerTypes[src_reg]);
      scalarType = GetScalarComponentType(srcType);
    }
    u32 result_type = GetTypeId(scalarType);

    // OpCompositeExtract: result_type result_id composite index...
    if (currentFunctionSize + 5 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (5 << 16) | spv::OpCompositeExtract;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = src_id;
    currentFunction[currentFunctionSize++] = component_idx;
    break;
  }

  case IR::OP_VEC_INSERT_DYNAMIC: {
    // Insert a scalar into a vector at a RUNTIME component index.
    // operand 0: source vector register
    // operand 1: value to insert
    // operand 2: index register (runtime-known)
    u16 src_vec_reg = ir->GetOperand(ir_idx, 0);
    u16 value_reg = ir->GetOperand(ir_idx, 1);
    u16 index_reg = ir->GetOperand(ir_idx, 2);
    u32 src_vec_id = GetSpirvId(src_vec_reg);
    u32 value_id = GetSpirvId(value_reg);
    u32 index_id = GetSpirvId(index_reg);
    u32 result_type = GetResultType(ir->destinations[ir_idx], src_vec_reg);
    Emit(spv::OpVectorInsertDynamic, result_type, dest, src_vec_id, value_id,
         index_id);
    break;
  }

  case IR::OP_VEC_INSERT: {
    // Insert a scalar into a vector at a specific component index
    // operand 0: source vector register
    // operand 1: component index (0-3 for x/y/z/w)
    // operand 2: value to insert (scalar)
    // Also used for element insertion into struct-field array values, where
    // the composite is an array and the index is the element index.
    u16 src_vec_reg = ir->GetOperand(ir_idx, 0);
    u32 src_vec_id = GetSpirvId(src_vec_reg);
    u32 component_idx = ir->GetOperand(ir_idx, 1);
    u16 value_reg = ir->GetOperand(ir_idx, 2);
    u32 value_id = GetSpirvId(value_reg);

    // Get result type from the source vector
    u32 result_type = GetResultType(ir->destinations[ir_idx], src_vec_reg);

    // Struct-field array value: the composite's real SPIR-V type is the
    // array type carried in the override, not the IR element CoreType.
    // Propagate the array marks so chained inserts/loads keep working.
    if (src_vec_reg < idCapacity && regIsStructArrayField[src_vec_reg]) {
      if (spirvTypeOverrides[src_vec_reg] != 0) {
        result_type = spirvTypeOverrides[src_vec_reg];
      }
      u16 dr = ir->destinations[ir_idx];
      if (dr < idCapacity) {
        regIsStructArrayField[dr] = true;
        spirvTypeOverrides[dr] = result_type;
        if (storagePtrElemTypes[src_vec_reg] != 0) {
          storagePtrElemTypes[dr] = storagePtrElemTypes[src_vec_reg];
        }
      }
    }

    // OpCompositeInsert: result_type result_id object composite index...
    // object = the value being inserted
    // composite = the vector/struct being modified
    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpCompositeInsert;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] =
        value_id; // object (value being inserted)
    currentFunction[currentFunctionSize++] =
        src_vec_id; // composite (vector being modified)
    currentFunction[currentFunctionSize++] = component_idx;
    break;
  }

  case IR::OP_VEC_SHUFFLE: {
    // Vector shuffle/swizzle for multi-component assignment
    // operand 0: first source vector (original)
    // operand 1: second source vector (value to insert)
    // metadata: packed shuffle indices (4 bits each, up to 4 components)
    u16 src0_reg = ir->GetOperand(ir_idx, 0);
    u16 src1_reg = ir->GetOperand(ir_idx, 1);
    u32 src0_id = GetSpirvId(src0_reg);
    u32 src1_id = GetSpirvId(src1_reg);
    u32 shuffleMask = ir->metadata[ir_idx];

    // Get result type from destination register
    u16 dest_reg = ir->destinations[ir_idx];

    // Determine number of components from result type
    CoreType destType = CoreType::FLOAT4;
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
    }
    u32 numComponents = 4;
    if (destType == CoreType::FLOAT3 || destType == CoreType::INT3 ||
        destType == CoreType::UINT3) {
      numComponents = 3;
    } else if (destType == CoreType::FLOAT2 || destType == CoreType::INT2 ||
               destType == CoreType::UINT2) {
      numComponents = 2;
    } else if (destType == CoreType::FLOAT || destType == CoreType::INT ||
               destType == CoreType::UINT || destType == CoreType::BOOL) {
      // Single component: use OpCompositeExtract instead of OpVectorShuffle
      numComponents = 1;
      u32 idx = shuffleMask & 0xF;
      u32 result_type = GetTypeId(destType);
      if (currentFunctionSize + 5 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (5 << 16) | spv::OpCompositeExtract;
      currentFunction[currentFunctionSize++] = result_type;
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = (idx < 4) ? src0_id : src1_id;
      currentFunction[currentFunctionSize++] = (idx < 4) ? idx : (idx - 4);
      break;
    } else if (destType == CoreType::CUSTOM || destType == CoreType::ENUM ||
               destType == CoreType::INVALID || destType == CoreType::VOID) {
      // For unknown types, try to infer from source vector type
      if (ir->registerTypes && src0_reg < ir->registerCount) {
        CoreType src0Type = static_cast<CoreType>(ir->registerTypes[src0_reg]);
        if (src0Type == CoreType::FLOAT3 || src0Type == CoreType::INT3 ||
            src0Type == CoreType::UINT3) {
          numComponents = 3;
        } else if (src0Type == CoreType::FLOAT2 || src0Type == CoreType::INT2 ||
                   src0Type == CoreType::UINT2) {
          numComponents = 2;
        }
      }
    }

    u32 result_type = GetResultType(dest_reg, src0_reg);

    // OpVectorShuffle: result_type result_id vec0 vec1 indices...
    u32 wordCount = 5 + numComponents;
    if (currentFunctionSize + wordCount > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (wordCount << 16) | spv::OpVectorShuffle;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = src0_id;
    currentFunction[currentFunctionSize++] = src1_id;

    // Unpack and emit component indices
    for (u32 i = 0; i < numComponents; i++) {
      u32 idx = (shuffleMask >> (i * 4)) & 0xF;
      currentFunction[currentFunctionSize++] = idx;
    }
    break;
  }

  // ========== Struct Operations ==========
  case IR::OP_STRUCT_EXTRACT: {
    // Extract a field from a struct: dest = struct.field
    // operand 0: source struct register
    // operand 1: field index
    // metadata: full 32-bit struct type hash
    u16 src_reg = ir->GetOperand(ir_idx, 0);
    u32 src_id = GetSpirvId(src_reg);
    u32 field_idx = ir->GetOperand(ir_idx, 1);

    // Get the struct type hash from metadata or register
    u32 metadata = ir->metadata[ir_idx];
    u32 struct_type_hash = metadata; // Full 32-bit hash
    if (struct_type_hash == 0 && src_reg < 512 && ir->registerStructTypes) {
      struct_type_hash = ir->registerStructTypes[src_reg];
    }

    // Determine result type from cached struct field types
    // (must match the type used when the struct was created in GetStructTypeId)
    u32 result_type = GetTypeId(CoreType::FLOAT); // Default
    bool isArrayField = false; // Track if this field is an array
    if (struct_type_hash != 0) {
      // Look up the struct in our cache to get the field type ID
      for (u32 i = 0; i < structTypeCount; i++) {
        if (structTypeHashes[i] == struct_type_hash) {
          if (field_idx < MAX_FIELDS_PER_STRUCT) {
            result_type =
                structFieldTypeIds[i * MAX_FIELDS_PER_STRUCT + field_idx];
          }
          break;
        }
      }
      // If not in cache, ensure the struct is created (will populate cache)
      if (result_type == 0) {
        GetStructTypeId(struct_type_hash);
        // Try again
        for (u32 i = 0; i < structTypeCount; i++) {
          if (structTypeHashes[i] == struct_type_hash) {
            if (field_idx < MAX_FIELDS_PER_STRUCT) {
              result_type =
                  structFieldTypeIds[i * MAX_FIELDS_PER_STRUCT + field_idx];
            }
            break;
          }
        }
      }
      // Check if this field is an array (from IR struct info)
      if (ir->structFieldArraySizes) {
        for (u32 i = 0; i < ir->structTypeCount; i++) {
          if (ir->structTypes[i].nameHash == struct_type_hash) {
            u32 fieldOffset = ir->structTypes[i].fieldOffset;
            if (field_idx < ir->structTypes[i].fieldCount &&
                ir->structFieldArraySizes[fieldOffset + field_idx] > 0) {
              isArrayField = true;
            }
            break;
          }
        }
      }
    }
    if (result_type == 0) {
      result_type = GetTypeId(CoreType::FLOAT);
    }

    // Check if source is a storage buffer variable (can't use
    // OpCompositeExtract)
    bool isStorageBufferVar = false;
    for (u32 b = 0; b < 32; b++) {
      if (storageBufferIds[b] == src_id) {
        isStorageBufferVar = true;
        break;
      }
    }

    u16 dest_reg = ir->destinations[ir_idx];

    if (isStorageBufferVar) {
      // Storage buffer - use OpAccessChain
      // Access pattern: buffer -> 0 (block member) -> 0 (first array element)
      // -> field_idx
      u32 field_ptr_type =
          GetPointerTypeId(result_type, spv::StorageClassStorageBuffer);
      u32 zero_id = GetIntConstantId(0, true);
      u32 field_id = GetIntConstantId(field_idx, false);

      if (isArrayField) {
        // Array field - leave as pointer for later indexing by OP_ARRAY_LOAD
        // The dest register will hold a pointer, not a value
        if (currentFunctionSize + 7 > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpAccessChain;
        currentFunction[currentFunctionSize++] = field_ptr_type;
        currentFunction[currentFunctionSize++] =
            dest; // Store pointer directly in dest
        currentFunction[currentFunctionSize++] = src_id;
        currentFunction[currentFunctionSize++] =
            zero_id; // Block wrapper member 0
        currentFunction[currentFunctionSize++] =
            zero_id; // First element in runtime array
        currentFunction[currentFunctionSize++] =
            field_id; // Field within struct

        // Track storage class for this pointer
        if (dest_reg < idCapacity) {
          storagePtrStorageClass[dest_reg] =
              static_cast<u32>(spv::StorageClassStorageBuffer);

          // Look up the element type from IR struct field types (not array
          // type) This is needed for OP_ARRAY_LOAD to know the element type
          for (u32 si = 0; si < ir->structTypeCount; si++) {
            if (ir->structTypes[si].nameHash == struct_type_hash) {
              u32 fieldOffset = ir->structTypes[si].fieldOffset;
              if (field_idx < ir->structTypes[si].fieldCount) {
                CoreType elemType = static_cast<CoreType>(
                    ir->structFieldTypes[fieldOffset + field_idx]);
                u32 elemTypeId = 0;
                if (elemType == CoreType::CUSTOM ||
                    elemType == CoreType::ENUM) {
                  // Struct element type - look up struct type hash
                  if (ir->structFieldTypeHashes) {
                    u32 elemStructHash =
                        ir->structFieldTypeHashes[fieldOffset + field_idx];
                    if (elemStructHash != 0) {
                      elemTypeId = GetStructTypeId(elemStructHash);
                    }
                  }
                } else {
                  elemTypeId = GetTypeId(elemType);
                }
                if (elemTypeId != 0) {
                  storagePtrElemTypes[dest_reg] = elemTypeId;
                }
              }
              break;
            }
          }
        }
      } else {
        // Non-array field - access and load
        u32 ptr_id = AllocateId();
        if (currentFunctionSize + 7 > currentFunctionCapacity) {
          GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpAccessChain;
        currentFunction[currentFunctionSize++] = field_ptr_type;
        currentFunction[currentFunctionSize++] = ptr_id;
        currentFunction[currentFunctionSize++] = src_id;
        currentFunction[currentFunctionSize++] =
            zero_id; // Block wrapper member 0
        currentFunction[currentFunctionSize++] =
            zero_id; // First element in runtime array
        currentFunction[currentFunctionSize++] =
            field_id; // Field within struct

        // Load from the pointer
        Emit(spv::OpLoad, result_type, dest, ptr_id);
      }
    } else {
      // Regular struct - use OpCompositeExtract
      if (currentFunctionSize + 5 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] =
          (5 << 16) | spv::OpCompositeExtract;
      currentFunction[currentFunctionSize++] = result_type;
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = src_id;
      currentFunction[currentFunctionSize++] = field_idx;
    }

    // Mark destination register if it holds a struct field array
    if (isArrayField && dest_reg < idCapacity) {
      regIsStructArrayField[dest_reg] = true;

      // For value extracts (OpCompositeExtract), the register's actual
      // SPIR-V type is the array type, not the IR's element CoreType.
      // Record the override so element inserts and copies type correctly.
      if (storagePtrStorageClass[dest_reg] == 0) {
        spirvTypeOverrides[dest_reg] = result_type;
      }

      // Track element type for ALL array fields (not just storage buffers)
      // This is needed for OP_ARRAY_LOAD to get the correct element type
      if (storagePtrElemTypes[dest_reg] == 0) { // Only if not already set
        for (u32 si = 0; si < ir->structTypeCount; si++) {
          if (ir->structTypes[si].nameHash == struct_type_hash) {
            u32 fieldOffset = ir->structTypes[si].fieldOffset;
            if (field_idx < ir->structTypes[si].fieldCount) {
              CoreType elemType = static_cast<CoreType>(
                  ir->structFieldTypes[fieldOffset + field_idx]);
              u32 elemTypeId = 0;
              if (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM) {
                if (ir->structFieldTypeHashes) {
                  u32 elemStructHash =
                      ir->structFieldTypeHashes[fieldOffset + field_idx];
                  if (elemStructHash != 0) {
                    elemTypeId = GetStructTypeId(elemStructHash);
                  }
                }
              } else {
                elemTypeId = GetTypeId(elemType);
              }
              if (elemTypeId != 0) {
                storagePtrElemTypes[dest_reg] = elemTypeId;
              }
            }
            break;
          }
        }
      }
    }
    break;
  }

  case IR::OP_STRUCT_INSERT: {
    // Insert a field into a struct: dest = struct with field=value
    // operand 0: source struct register
    // operand 1: field index
    // operand 2: value to insert
    // metadata: full 32-bit struct type hash
    u16 struct_reg = ir->GetOperand(ir_idx, 0);
    u32 struct_id = GetSpirvId(struct_reg);
    u32 field_idx = ir->GetOperand(ir_idx, 1);
    u16 value_reg = ir->GetOperand(ir_idx, 2);
    u32 value_id = GetSpirvId(value_reg);

    // Get struct type for result type
    u32 metadata = ir->metadata[ir_idx];
    u32 struct_type_hash = metadata; // Full 32-bit hash
    if (struct_type_hash == 0 && struct_reg < 512 && ir->registerStructTypes) {
      struct_type_hash = ir->registerStructTypes[struct_reg];
    }

    u32 result_type = GetStructTypeId(struct_type_hash);
    if (result_type == 0) {
      // Fallback: just copy the struct (shouldn't happen)
      if (currentFunctionSize + 4 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] = (4 << 16) | spv::OpCopyObject;
      currentFunction[currentFunctionSize++] = GetTypeId(CoreType::FLOAT4);
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = struct_id;
      break;
    }

    // OpCompositeInsert: result_type result_id object composite indices...
    if (currentFunctionSize + 6 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpCompositeInsert;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = value_id;  // Object to insert
    currentFunction[currentFunctionSize++] = struct_id; // Composite
    currentFunction[currentFunctionSize++] = field_idx; // Index
    break;
  }

  case IR::OP_STRUCT_ARRAY_EXTRACT:
  case IR::OP_STRUCT_ARRAY_INSERT: {
    // Fused two-level access to an array field held by value in a struct:
    //   EXTRACT: dest = struct.field[index]        (element value)
    //   INSERT:  dest = struct with field[index]=value (new struct value)
    // operand 0: struct register, operand 1: field index (literal),
    // operand 2: element index (register or encoded constant),
    // operand 3 (INSERT): value register. metadata: struct type hash.
    // Constant indices use multi-index OpCompositeExtract/Insert; dynamic
    // indices spill the struct into a pre-declared Function-storage scratch
    // variable and go through OpAccessChain.
    u16 struct_reg = ir->GetOperand(ir_idx, 0);
    u32 struct_id = GetSpirvId(struct_reg);
    u32 field_idx = ir->GetOperand(ir_idx, 1);
    u16 index_reg = ir->GetOperand(ir_idx, 2);
    bool isInsert = (op == IR::OP_STRUCT_ARRAY_INSERT);

    u32 struct_type_hash = ir->metadata[ir_idx];
    if (struct_type_hash == 0 && struct_reg < 512 && ir->registerStructTypes) {
      struct_type_hash = ir->registerStructTypes[struct_reg];
    }
    u32 struct_type_id =
        struct_type_hash != 0 ? GetStructTypeId(struct_type_hash) : 0;

    // Element type from the IR struct field metadata
    u32 elem_type_id = 0;
    for (u32 si = 0; si < ir->structTypeCount; si++) {
      if (ir->structTypes[si].nameHash != struct_type_hash) {
        continue;
      }
      u32 fieldOffset = ir->structTypes[si].fieldOffset;
      if (field_idx < ir->structTypes[si].fieldCount) {
        CoreType elemType = static_cast<CoreType>(
            ir->structFieldTypes[fieldOffset + field_idx]);
        if (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM) {
          if (ir->structFieldTypeHashes) {
            u32 elemStructHash =
                ir->structFieldTypeHashes[fieldOffset + field_idx];
            if (elemStructHash != 0) {
              elem_type_id = GetStructTypeId(elemStructHash);
            }
          }
        } else {
          elem_type_id = GetTypeId(elemType);
        }
      }
      break;
    }
    if (elem_type_id == 0) {
      elem_type_id = GetTypeId(CoreType::FLOAT);
    }
    if (struct_type_id == 0) {
      Emit(spv::OpUndef, isInsert ? GetTypeId(CoreType::FLOAT) : elem_type_id,
           dest);
      break;
    }

    u32 value_id = isInsert ? GetSpirvId(ir->GetOperand(ir_idx, 3)) : 0;

    bool isConstIndex = (index_reg & 0xC000) == 0x4000;
    if (isConstIndex) {
      u32 slot = index_reg & 0x3FFF;
      u32 index_val = ir->intConstants[slot];
      if (isInsert) {
        Emit(spv::OpCompositeInsert, struct_type_id, dest, value_id,
             struct_id, field_idx, index_val);
      } else {
        Emit(spv::OpCompositeExtract, elem_type_id, dest, struct_id,
             field_idx, index_val);
      }
    } else {
      u32 scratch_var = GetStructArrayScratchVar(struct_type_id);
      if (scratch_var == 0) {
        // No scratch variable pre-declared - keep the module well-formed
        Emit(spv::OpUndef, isInsert ? struct_type_id : elem_type_id, dest);
        break;
      }
      u32 index_id = GetSpirvId(index_reg);
      u32 field_const = GetIntConstantId(field_idx, true);
      u32 elem_ptr_type =
          GetPointerTypeId(elem_type_id, spv::StorageClassFunction);
      Emit(spv::OpStore, scratch_var, struct_id);
      u32 elem_ptr = AllocateId();
      Emit(spv::OpAccessChain, elem_ptr_type, elem_ptr, scratch_var,
           field_const, index_id);
      if (isInsert) {
        Emit(spv::OpStore, elem_ptr, value_id);
        Emit(spv::OpLoad, struct_type_id, dest, scratch_var);
      } else {
        Emit(spv::OpLoad, elem_type_id, dest, elem_ptr);
      }
    }
    break;
  }

  case IR::OP_STRUCT_CONSTRUCT: {
    // Build struct from field values: dest = struct(f0, f1, f2...)
    // Uses all 4 operand slots for field values
    // metadata: struct type hash
    u32 struct_type_hash = ir->metadata[ir_idx];
    u32 result_type = GetStructTypeId(struct_type_hash);

    if (result_type == 0) {
      // Unknown struct type - emit placeholder
      break;
    }

    // Count field values from operands (non-0xFFFF values)
    u32 fieldIds[4] = {0, 0, 0, 0};
    u32 fieldCount = 0;
    for (u32 i = 0; i < 4; i++) {
      u16 op_reg = ir->GetOperand(ir_idx, i);
      if (op_reg == 0xFFFF)
        continue;
      fieldIds[fieldCount++] = GetSpirvId(op_reg);
    }

    // OpCompositeConstruct: result_type result_id constituents...
    u32 wordCount = 3 + fieldCount;
    if (currentFunctionSize + wordCount > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (wordCount << 16) | spv::OpCompositeConstruct;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    for (u32 i = 0; i < fieldCount; i++) {
      currentFunction[currentFunctionSize++] = fieldIds[i];
    }
    break;
  }

  case IR::OP_ENUM_CONSTRUCT: {
    // Build enum variant: dest = EnumType::Variant(args...)
    // metadata: (variantIndex << 16) | (argCount << 8) | (enumHash low bits)
    // operands: variant field values
    u32 metadata = ir->metadata[ir_idx];
    u32 variantIndex = (metadata >> 16) & 0xFFFF;
    u32 argCount = (metadata >> 8) & 0xFF;

    // Get enum struct type from register struct type info
    u16 dest_reg = ir->destinations[ir_idx];
    u32 enumStructHash = 0;
    if (dest_reg < ir->registerCount && ir->registerStructTypes) {
      enumStructHash = ir->registerStructTypes[dest_reg];
    }

    u32 result_type = GetStructTypeId(enumStructHash);
    if (result_type == 0) {
      break;
    }

    // Build constituents: [tag, field0, field1, ...]
    const IR::IRProgram::StructTypeInfo *enumStructInfo = nullptr;
    for (u32 s = 0; s < ir->structTypeCount; s++) {
      if (ir->structTypes[s].nameHash == enumStructHash) {
        enumStructInfo = &ir->structTypes[s];
        break;
      }
    }
    u32 totalFieldCount = enumStructInfo ? enumStructInfo->fieldCount : 1;
    u32 fieldOffset = enumStructInfo ? enumStructInfo->fieldOffset : 0;

    auto fieldTypeAt = [&](u32 actualFieldIndex) -> CoreType {
      if (!enumStructInfo || actualFieldIndex >= enumStructInfo->fieldCount) {
        return CoreType::FLOAT;
      }
      return static_cast<CoreType>(
          ir->structFieldTypes[fieldOffset + actualFieldIndex]);
    };

    auto fieldTypeHashAt = [&](u32 actualFieldIndex) -> u32 {
      if (!enumStructInfo || !ir->structFieldTypeHashes ||
          actualFieldIndex >= enumStructInfo->fieldCount) {
        return 0;
      }
      return ir->structFieldTypeHashes[fieldOffset + actualFieldIndex];
    };

    auto defaultValueForField = [&](u32 actualFieldIndex) -> u32 {
      CoreType fieldType = fieldTypeAt(actualFieldIndex);
      switch (fieldType) {
      case CoreType::INT:
        return GetIntConstantId(0, false);
      case CoreType::UINT:
        return GetIntConstantId(0, true);
      case CoreType::BOOL:
        return GetBoolConstantId(false);
      case CoreType::CUSTOM:
      case CoreType::ENUM: {
        u32 fieldHash = fieldTypeHashAt(actualFieldIndex);
        u32 fieldTypeId = fieldHash != 0 ? GetStructTypeId(fieldHash) : 0;
        if (fieldTypeId == 0) {
          fieldTypeId = GetTypeId(CoreType::FLOAT);
        }
        u32 undefId = AllocateId();
        Emit(spv::OpUndef, fieldTypeId, undefId);
        return undefId;
      }
      default:
        return GetFloatConstantId(0.0f);
      }
    };

    // Prepare constituents
    u32 constituents[128] = {0};
    u32 constituentCount = 0;

    // First constituent: tag (variant index)
    constituents[constituentCount++] = GetIntConstantId(variantIndex, false);

    // Remaining constituents: field values from operands, padded with zeros.
    // Vector operands and flattened struct operands are decomposed into the
    // enum payload storage fields. Nested enum payloads are kept as aggregate
    // struct fields when the destination field type is CUSTOM.
    for (u32 i = 0; i < 4 && constituentCount < totalFieldCount; i++) {
      u16 op_reg = ir->GetOperand(ir_idx, i);
      if (op_reg != 0x3FFF && op_reg != 0xFFFF) {
        // Check if this operand is a vector type that needs decomposition
        CoreType opType = CoreType::FLOAT;
        if (ir->registerTypes && op_reg < ir->registerCount) {
          opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
        }

        CoreType expectedFieldType = fieldTypeAt(constituentCount);
        u32 opStructHash = 0;
        if ((opType == CoreType::CUSTOM || opType == CoreType::ENUM) &&
            op_reg < ir->registerCount && ir->registerStructTypes) {
          opStructHash = ir->registerStructTypes[op_reg];
        }

        if ((opType == CoreType::CUSTOM || opType == CoreType::ENUM) &&
            expectedFieldType != CoreType::CUSTOM && opStructHash != 0) {
          const IR::IRProgram::StructTypeInfo *operandStructInfo = nullptr;
          for (u32 s = 0; s < ir->structTypeCount; s++) {
            if (ir->structTypes[s].nameHash == opStructHash) {
              operandStructInfo = &ir->structTypes[s];
              break;
            }
          }

          if (operandStructInfo) {
            u32 compositeId = GetSpirvId(op_reg);
            for (u32 f = 0; f < operandStructInfo->fieldCount &&
                            constituentCount < totalFieldCount;
                 f++) {
              CoreType subFieldType = static_cast<CoreType>(
                  ir->structFieldTypes[operandStructInfo->fieldOffset + f]);
              u32 subFieldTypeId = 0;
              if (subFieldType == CoreType::CUSTOM ||
                  subFieldType == CoreType::ENUM) {
                u32 subHash = ir->structFieldTypeHashes
                                  ? ir->structFieldTypeHashes
                                        [operandStructInfo->fieldOffset + f]
                                  : 0;
                subFieldTypeId = subHash != 0 ? GetStructTypeId(subHash) : 0;
              } else {
                subFieldTypeId = GetTypeId(subFieldType);
              }
              if (subFieldTypeId == 0) {
                subFieldTypeId = GetTypeId(CoreType::FLOAT);
              }

              u32 fieldId = AllocateId();
              Emit(spv::OpCompositeExtract, subFieldTypeId, fieldId,
                   compositeId, f);

              u32 componentCount = 1;
              switch (subFieldType) {
              case CoreType::FLOAT2:
              case CoreType::INT2:
              case CoreType::UINT2:
                componentCount = 2;
                break;
              case CoreType::FLOAT3:
              case CoreType::INT3:
              case CoreType::UINT3:
                componentCount = 3;
                break;
              case CoreType::FLOAT4:
              case CoreType::INT4:
              case CoreType::UINT4:
                componentCount = 4;
                break;
              default:
                componentCount = 1;
                break;
              }

              if (componentCount == 1) {
                constituents[constituentCount++] = fieldId;
              } else {
                CoreType scalarType = GetScalarComponentType(subFieldType);
                u32 scalarTypeId = GetTypeId(scalarType);
                for (u32 c = 0; c < componentCount &&
                                constituentCount < totalFieldCount;
                     c++) {
                  u32 extractId = AllocateId();
                  Emit(spv::OpCompositeExtract, scalarTypeId, extractId,
                       fieldId, c);
                  constituents[constituentCount++] = extractId;
                }
              }
            }
            continue;
          }
        }

        u32 componentCount = 1;
        switch (opType) {
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
          componentCount = 2;
          break;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
          componentCount = 3;
          break;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
          componentCount = 4;
          break;
        default:
          componentCount = 1;
          break;
        }

        if (componentCount == 1) {
          // Scalar - use directly
          constituents[constituentCount++] = GetSpirvId(op_reg);
        } else {
          // Vector - extract each component
          u32 vec_id = GetSpirvId(op_reg);
          u32 scalar_type = GetTypeId(GetScalarComponentType(opType));
          for (u32 c = 0;
               c < componentCount && constituentCount < totalFieldCount; c++) {
            u32 extract_id = AllocateId();
            // OpCompositeExtract: result_type result_id composite index
            Emit(spv::OpCompositeExtract, scalar_type, extract_id, vec_id, c);
            constituents[constituentCount++] = extract_id;
          }
        }
      } else if (i < argCount) {
        // Unexpected sentinel - use a type-correct default
        constituents[constituentCount++] =
            defaultValueForField(constituentCount);
      } else {
        // Padding
        constituents[constituentCount++] =
            defaultValueForField(constituentCount);
      }
    }

    // Pad remaining fields
    while (constituentCount < totalFieldCount) {
      constituents[constituentCount++] = defaultValueForField(constituentCount);
    }

    // OpCompositeConstruct: result_type result_id constituents...
    u32 wordCount = 3 + constituentCount;
    if (currentFunctionSize + wordCount > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (wordCount << 16) | spv::OpCompositeConstruct;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    for (u32 i = 0; i < constituentCount; i++) {
      currentFunction[currentFunctionSize++] = constituents[i];
    }
    break;
  }

  case IR::OP_ENUM_TAG: {
    // Extract tag from enum: dest = enum.tag (field 0)
    u16 enum_reg = ir->GetOperand(ir_idx, 0);
    u32 enum_id = GetSpirvId(enum_reg);

    u32 result_type = GetTypeId(CoreType::INT);

    // OpCompositeExtract: result_type result_id composite index...
    if (currentFunctionSize + 5 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (5 << 16) | spv::OpCompositeExtract;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = enum_id;
    currentFunction[currentFunctionSize++] = 0; // Index 0 = tag field
    break;
  }

  case IR::OP_ENUM_FIELD: {
    // Extract field from enum: dest = enum.field[N]
    // operand 0: enum register
    // operand 1: field index (0-based into variant data, so actual index is
    // 1+N)
    u16 enum_reg = ir->GetOperand(ir_idx, 0);
    u16 field_idx_reg = ir->GetOperand(ir_idx, 1);
    u32 enum_id = GetSpirvId(enum_reg);
    u16 dest_reg = ir->destinations[ir_idx];

    // For now, assume field index is a constant
    u32 field_idx = 0;
    if (field_idx_reg & 0x4000) {
      // Int constant
      u32 idx = field_idx_reg & 0x3FFF;
      field_idx = ir->intConstants[idx];
    }

    u32 result_type = 0;
    if (dest_reg < ir->registerCount && ir->registerTypes) {
      CoreType destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if ((destType == CoreType::CUSTOM || destType == CoreType::ENUM) &&
          ir->registerStructTypes) {
        u32 destStructHash = ir->registerStructTypes[dest_reg];
        if (destStructHash != 0) {
          result_type = GetStructTypeId(destStructHash);
        }
      } else {
        result_type = GetTypeId(destType);
      }
    }

    if (result_type == 0 && enum_reg < ir->registerCount &&
        ir->registerStructTypes) {
      u32 enumStructHash = ir->registerStructTypes[enum_reg];
      for (u32 s = 0; s < ir->structTypeCount; s++) {
        if (ir->structTypes[s].nameHash != enumStructHash) {
          continue;
        }
        const IR::IRProgram::StructTypeInfo &info = ir->structTypes[s];
        u32 actualFieldIndex = 1 + field_idx;
        if (actualFieldIndex >= info.fieldCount) {
          break;
        }
        CoreType fieldType = static_cast<CoreType>(
            ir->structFieldTypes[info.fieldOffset + actualFieldIndex]);
        if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
            ir->structFieldTypeHashes) {
          u32 fieldStructHash =
              ir->structFieldTypeHashes[info.fieldOffset + actualFieldIndex];
          if (fieldStructHash != 0) {
            result_type = GetStructTypeId(fieldStructHash);
          }
        } else {
          result_type = GetTypeId(fieldType);
        }
        break;
      }
    }

    if (result_type == 0) {
      result_type = GetTypeId(CoreType::FLOAT);
    }

    // OpCompositeExtract: result_type result_id composite index...
    // Field index is offset by 1 because field 0 is the tag
    if (currentFunctionSize + 5 > currentFunctionCapacity) {
      GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] =
        (5 << 16) | spv::OpCompositeExtract;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = enum_id;
    currentFunction[currentFunctionSize++] =
        1 + field_idx; // Offset by 1 for tag
    break;
  }

  case IR::OP_ARRAY_LOAD: {
    // Load element from array: dest = array[index]
    // operand 0: base array register
    // operand 1: index register
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 index_reg = ir->GetOperand(ir_idx, 1);
    u32 base_id = GetSpirvId(base_reg);
    u32 index_id = GetSpirvId(index_reg);

    // Get the element type from the destination register
    CoreType elemType = CoreType::FLOAT;
    u16 dest_reg = ir->destinations[ir_idx];
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        elemType = regType;
      }
    }
    u32 elem_type_id = GetTypeId(elemType);
    if (elem_type_id == 0 &&
        (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM)) {
      u32 structHash = 0;
      if (ir->registerStructTypes && dest_reg < ir->registerCount) {
        structHash = ir->registerStructTypes[dest_reg];
      }
      if (structHash == 0 && ir->registerStructTypes &&
          base_reg < ir->registerCount) {
        structHash = ir->registerStructTypes[base_reg];
      }
      // Also check localArrayStructTypes for local arrays
      if (structHash == 0 && ir->localArrayStructTypes &&
          ir->registerStorageInfo && base_reg < ir->registerCount &&
          (ir->registerStorageInfo[base_reg] &
           IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) {
        u32 arrayIdx = (ir->registerStorageInfo[base_reg] >>
                        IR::IRProgram::STORAGE_BINDING_SHIFT);
        if (arrayIdx < ir->localArrayCount) {
          structHash = ir->localArrayStructTypes[arrayIdx];
        }
      }
      if (structHash != 0) {
        elem_type_id = GetStructTypeId(structHash);
      }
    }
    if (elem_type_id == 0) {
      elem_type_id = GetTypeId(CoreType::FLOAT);
    }

    // Check if this is a storage buffer pointer
    bool isStoragePtr =
        ir->registerStorageInfo && base_reg < ir->registerCount &&
        (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_PTR);

    // Check if this is a local array
    bool isLocalArray = ir->registerStorageInfo &&
                        base_reg < ir->registerCount &&
                        (ir->registerStorageInfo[base_reg] &
                         IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

    if (isLocalArray) {
      // Local array with Function storage class
      u32 elem_ptr_type =
          GetPointerTypeId(elem_type_id, spv::StorageClassFunction);
      u32 ptr_id = AllocateId();
      Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
      Emit(spv::OpLoad, elem_type_id, dest, ptr_id);
    } else if (isStoragePtr) {
      // Storage buffer or workgroup array
      spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
      if (ir->registerStorageInfo[base_reg] &
          IR::IRProgram::STORAGE_IS_SHARED) {
        storageClass = spv::StorageClassWorkgroup;
      }
      u32 elem_ptr_type = GetPointerTypeId(elem_type_id, storageClass);
      u32 ptr_id = AllocateId();

      bool isStorageBufferVar = false;
      if (storageClass == spv::StorageClassStorageBuffer) {
        for (u32 b = 0; b < 32; b++) {
          if (storageBufferIds[b] == base_id) {
            isStorageBufferVar = true;
            break;
          }
        }
      }

      if (isStorageBufferVar) {
        u32 zero_id = GetIntConstantId(0, true);
        Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, zero_id,
             index_id);
      } else {
        Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
      }
      Emit(spv::OpLoad, elem_type_id, dest, ptr_id);
    } else {
      // Check if base is a struct field array (not a matrix/vector)
      bool isStructArrayField =
          base_reg < idCapacity && regIsStructArrayField[base_reg];

      // Check if base is a matrix type - need to extract column
      CoreType baseType = CoreType::FLOAT;
      if (ir->registerTypes && base_reg < ir->registerCount) {
        baseType = static_cast<CoreType>(ir->registerTypes[base_reg]);
      }

      if (isStructArrayField) {
        // Array element extraction from struct field array
        // Check if base is a storage buffer pointer (from OP_STRUCT_EXTRACT on
        // storage buffer)
        bool isStoragePtr2 =
            base_reg < idCapacity && storagePtrStorageClass[base_reg] != 0;

        // Use tracked element type from OP_STRUCT_EXTRACT if available,
        // otherwise fall back to destination register type
        u32 array_elem_type_id = elem_type_id;
        if (base_reg < idCapacity && storagePtrElemTypes[base_reg] != 0) {
          array_elem_type_id = storagePtrElemTypes[base_reg];
        }

        if (isStoragePtr2) {
          // Storage buffer pointer - use OpAccessChain + OpLoad
          spv::StorageClass storageClass =
              static_cast<spv::StorageClass>(storagePtrStorageClass[base_reg]);
          u32 elem_ptr_type =
              GetPointerTypeId(array_elem_type_id, storageClass);
          u32 ptr_id = AllocateId();
          Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
          Emit(spv::OpLoad, array_elem_type_id, dest, ptr_id);
        } else {
          // Regular array value - use OpCompositeExtract or OpUndef.
          // (Dynamic indexing of struct array fields is handled by the
          // fused OP_STRUCT_ARRAY_EXTRACT path; this fallback only sees
          // array values from non-fused shapes.)
          bool isConstIndex = (index_reg & 0xC000) == 0x4000;
          if (isConstIndex) {
            u32 idx = index_reg & 0x3FFF;
            u32 index_val = ir->intConstants[idx];
            // OpCompositeExtract for constant index
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
              GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] =
                (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = array_elem_type_id;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = base_id;
            currentFunction[currentFunctionSize++] = index_val;
          } else {
            // Dynamic index on non-storage array - emit OpUndef for now
            Emit(spv::OpUndef, array_elem_type_id, dest);
          }
        }

        // Store the actual SPIR-V type for this register so downstream
        // operations use the correct type (not the IR's column type)
        if (dest_reg < idCapacity) {
          spirvTypeOverrides[dest_reg] = array_elem_type_id;
        }
      } else if (baseType == CoreType::MAT2 || baseType == CoreType::MAT3 ||
                 baseType == CoreType::MAT4) {
        // Matrix column extraction using OpCompositeExtract (for constant
        // index) or OpVectorExtractDynamic (for dynamic index)
        CoreType columnType = (baseType == CoreType::MAT2)   ? CoreType::FLOAT2
                              : (baseType == CoreType::MAT3) ? CoreType::FLOAT3
                                                             : CoreType::FLOAT4;
        u32 column_type_id = GetTypeId(columnType);

        // Check if index is a constant
        bool isConstIndex = (index_reg & 0xC000) == 0x4000;
        if (isConstIndex) {
          u32 idx = index_reg & 0x3FFF;
          u32 index_val = ir->intConstants[idx];
          // OpCompositeExtract for constant index
          if (currentFunctionSize + 5 > currentFunctionCapacity) {
            GrowCurrentFunction();
          }
          currentFunction[currentFunctionSize++] =
              (5 << 16) | spv::OpCompositeExtract;
          currentFunction[currentFunctionSize++] = column_type_id;
          currentFunction[currentFunctionSize++] = dest;
          currentFunction[currentFunctionSize++] = base_id;
          currentFunction[currentFunctionSize++] = index_val;
        } else {
          // Dynamic column index: extract each column with a constant index
          // then chain OpSelect to pick the right one. SPIR-V has no direct
          // "dynamic matrix column" op, and Function-local variables must be
          // declared in the entry block, so this inline chain is simplest.
          u32 columnCount = (baseType == CoreType::MAT2)   ? 2
                            : (baseType == CoreType::MAT3) ? 3
                                                           : 4;
          u32 col_ids[4];
          for (u32 c = 0; c < columnCount; ++c) {
            col_ids[c] = AllocateId();
            Emit(spv::OpCompositeExtract, column_type_id, col_ids[c], base_id,
                 c);
          }
          u32 bool_type = GetTypeId(CoreType::BOOL);
          // OpSelect with a vector result in SPIR-V <1.4 requires a vector
          // condition of matching component count; broadcast the scalar bool.
          CoreType bvecType = (baseType == CoreType::MAT2)   ? CoreType::BOOL2
                              : (baseType == CoreType::MAT3) ? CoreType::BOOL3
                                                             : CoreType::BOOL4;
          u32 bvec_type_id = GetTypeId(bvecType);
          u32 result = col_ids[columnCount - 1];
          for (u32 c = columnCount - 1; c > 0; --c) {
            u32 const_id = GetIntConstantId(c - 1, true);
            u32 eq_id = AllocateId();
            Emit(spv::OpIEqual, bool_type, eq_id, index_id, const_id);
            u32 eq_vec_id = AllocateId();
            if (columnCount == 2) {
              Emit(spv::OpCompositeConstruct, bvec_type_id, eq_vec_id, eq_id,
                   eq_id);
            } else if (columnCount == 3) {
              Emit(spv::OpCompositeConstruct, bvec_type_id, eq_vec_id, eq_id,
                   eq_id, eq_id);
            } else {
              Emit(spv::OpCompositeConstruct, bvec_type_id, eq_vec_id, eq_id,
                   eq_id, eq_id, eq_id);
            }
            u32 sel_id = (c == 1) ? dest : AllocateId();
            Emit(spv::OpSelect, column_type_id, sel_id, eq_vec_id,
                 col_ids[c - 1], result);
            result = sel_id;
          }
        }
      } else if (baseType == CoreType::FLOAT2 || baseType == CoreType::FLOAT3 ||
                 baseType == CoreType::FLOAT4) {
        // Vector element extraction
        bool isConstIndex = (index_reg & 0xC000) == 0x4000;
        if (isConstIndex) {
          u32 idx = index_reg & 0x3FFF;
          u32 index_val = ir->intConstants[idx];
          // OpCompositeExtract for constant index
          if (currentFunctionSize + 5 > currentFunctionCapacity) {
            GrowCurrentFunction();
          }
          currentFunction[currentFunctionSize++] =
              (5 << 16) | spv::OpCompositeExtract;
          currentFunction[currentFunctionSize++] = elem_type_id;
          currentFunction[currentFunctionSize++] = dest;
          currentFunction[currentFunctionSize++] = base_id;
          currentFunction[currentFunctionSize++] = index_val;
        } else {
          // Dynamic vector index
          Emit(spv::OpVectorExtractDynamic, elem_type_id, dest, base_id,
               index_id);
        }
      } else {
        // Non-storage arrays are lowered as placeholder values
        Emit(spv::OpUndef, elem_type_id, dest);
      }
    }
    break;
  }

  case IR::OP_ARRAY_STORE: {
    // Store element to array: array[index] = value
    // dest: base array register
    // operand 0: index register
    // operand 1: value register
    u16 base_reg = ir->destinations[ir_idx];
    u16 index_reg = ir->GetOperand(ir_idx, 0);
    u16 value_reg = ir->GetOperand(ir_idx, 1);

    bool isStoragePtr =
        ir->registerStorageInfo && base_reg < ir->registerCount &&
        (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_PTR);

    // Check if this is a local array
    bool isLocalArray = ir->registerStorageInfo &&
                        base_reg < ir->registerCount &&
                        (ir->registerStorageInfo[base_reg] &
                         IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

    if (isLocalArray) {
      // Local array with Function storage class
      CoreType elemType = CoreType::FLOAT;
      // First check if value_reg is a constant - infer type from encoding
      if ((value_reg & 0xC000) == 0xC000) {
        elemType = CoreType::BOOL;
      } else if (value_reg & 0x8000) {
        elemType = CoreType::FLOAT;
      } else if (value_reg & 0x4000) {
        elemType = CoreType::INT;
      } else if (value_reg & 0x2000) {
        elemType = CoreType::UINT;
      } else if (ir->registerTypes && value_reg < ir->registerCount) {
        CoreType regType = static_cast<CoreType>(ir->registerTypes[value_reg]);
        if (regType != CoreType::VOID && regType != CoreType::INVALID) {
          elemType = regType;
        }
      }
      u32 elem_type_id = GetTypeId(elemType);

      // Handle struct element types for local arrays
      if (elem_type_id == 0 &&
          (elemType == CoreType::CUSTOM || elemType == CoreType::INVALID)) {
        // Try to get struct type from value register
        u32 structHash = 0;
        if (ir->registerStructTypes && value_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[value_reg];
        }
        // If not found, try localArrayStructTypes
        if (structHash == 0 && ir->localArrayStructTypes) {
          u32 arrayIdx = (ir->registerStorageInfo[base_reg] >>
                          IR::IRProgram::STORAGE_BINDING_SHIFT);
          if (arrayIdx < ir->localArrayCount) {
            structHash = ir->localArrayStructTypes[arrayIdx];
          }
        }
        if (structHash != 0) {
          elem_type_id = GetStructTypeId(structHash);
        }
      }
      if (elem_type_id == 0) {
        elem_type_id = GetTypeId(CoreType::FLOAT);
      }
      u32 elem_ptr_type =
          GetPointerTypeId(elem_type_id, spv::StorageClassFunction);
      u32 base_id = GetSpirvId(base_reg);
      u32 index_id = GetSpirvId(index_reg);
      u32 value_id = GetSpirvId(value_reg);

      u32 ptr_id = AllocateId();
      Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
      Emit(spv::OpStore, ptr_id, value_id);
    } else if (isStoragePtr) {
      spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
      if (ir->registerStorageInfo[base_reg] &
          IR::IRProgram::STORAGE_IS_SHARED) {
        storageClass = spv::StorageClassWorkgroup;
      }

      CoreType elemType = CoreType::FLOAT;
      // First check if value_reg is a constant - infer type from encoding
      if ((value_reg & 0xC000) == 0xC000) {
        elemType = CoreType::BOOL;
      } else if (value_reg & 0x8000) {
        elemType = CoreType::FLOAT;
      } else if (value_reg & 0x4000) {
        elemType = CoreType::INT;
      } else if (value_reg & 0x2000) {
        elemType = CoreType::UINT;
      } else if (ir->registerTypes && value_reg < ir->registerCount) {
        CoreType regType = static_cast<CoreType>(ir->registerTypes[value_reg]);
        if (regType != CoreType::VOID && regType != CoreType::INVALID) {
          elemType = regType;
        }
      } else if (ir->registerTypes && base_reg < ir->registerCount) {
        CoreType regType = static_cast<CoreType>(ir->registerTypes[base_reg]);
        if (regType != CoreType::VOID && regType != CoreType::INVALID) {
          elemType = regType;
        }
      }
      u32 elem_type_id = GetTypeId(elemType);
      if (elem_type_id == 0 &&
          (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM) &&
          ir->registerStructTypes) {
        u32 structHash = 0;
        if (value_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[value_reg];
        }
        if (structHash == 0 && base_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[base_reg];
        }
        if (structHash != 0) {
          elem_type_id = GetStructTypeId(structHash);
        }
      }
      if (elem_type_id == 0) {
        elem_type_id = GetTypeId(CoreType::FLOAT);
      }
      u32 elem_ptr_type = GetPointerTypeId(elem_type_id, storageClass);
      u32 base_id = GetSpirvId(base_reg);
      u32 index_id = GetSpirvId(index_reg);
      u32 value_id = GetSpirvId(value_reg);

      u32 ptr_id = AllocateId();

      bool isStorageBufferVar = false;
      if (storageClass == spv::StorageClassStorageBuffer) {
        for (u32 b = 0; b < 32; b++) {
          if (storageBufferIds[b] == base_id) {
            isStorageBufferVar = true;
            break;
          }
        }
      }

      if (isStorageBufferVar) {
        u32 zero_id = GetIntConstantId(0, true);
        Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, zero_id,
             index_id);
      } else {
        Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
      }
      Emit(spv::OpStore, ptr_id, value_id);
    } else {
      // For local arrays (not storage), define the base register as a simple
      // assignment. This keeps SSA values defined even though full array
      // semantics are not implemented.
      u32 type_id = 0;

      // Check for SPIR-V type override on value register (e.g., from struct
      // array extraction)
      if (value_reg < idCapacity && spirvTypeOverrides[value_reg] != 0) {
        type_id = spirvTypeOverrides[value_reg];
      }

      if (type_id == 0) {
        CoreType elemType = CoreType::FLOAT;
        if (ir->registerTypes && value_reg < ir->registerCount) {
          CoreType regType =
              static_cast<CoreType>(ir->registerTypes[value_reg]);
          if (regType != CoreType::VOID && regType != CoreType::INVALID) {
            elemType = regType;
          }
        } else if (ir->registerTypes && base_reg < ir->registerCount) {
          CoreType regType = static_cast<CoreType>(ir->registerTypes[base_reg]);
          if (regType != CoreType::VOID && regType != CoreType::INVALID) {
            elemType = regType;
          }
        }

        type_id = GetTypeId(elemType);
        if (type_id == 0 &&
            (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM) &&
            ir->registerStructTypes) {
          u32 structHash = 0;
          if (value_reg < ir->registerCount) {
            structHash = ir->registerStructTypes[value_reg];
          }
          if (structHash == 0 && base_reg < ir->registerCount) {
            structHash = ir->registerStructTypes[base_reg];
          }
          if (structHash != 0) {
            type_id = GetStructTypeId(structHash);
          }
        }
        if (type_id == 0) {
          type_id = GetTypeId(CoreType::FLOAT);
        }
      }
      u32 value_id = GetSpirvId(value_reg);
      u32 result_id = 0;
      if (base_reg < idCapacity && hasPreAllocatedId[base_reg]) {
        result_id = spirvIds[base_reg];
        hasPreAllocatedId[base_reg] = false;
      } else if (base_reg < idCapacity) {
        result_id = AllocateId();
        spirvIds[base_reg] = result_id;
      }

      if (result_id != 0) {
        Emit(spv::OpCopyObject, type_id, result_id, value_id);
      }
    }
    break;
  }

    // ========== Storage Buffer Access Chain Operations ==========
    // These ops maintain pointer semantics for proper SPIR-V OpAccessChain
    // generation

  case IR::OP_STORAGE_PTR: {
    // Get storage buffer base pointer: dest = &buffer
    // operand0 = binding index
    u16 binding = ir->GetOperand(ir_idx, 0);

    // Get the storage buffer variable ID
    u32 ssbo_var_id = storageBufferIds[binding];
    if (ssbo_var_id == 0) {
      // Storage buffer not declared - emit undef
      u32 type_id = GetTypeId(CoreType::UINT);
      Emit(spv::OpUndef, type_id, dest);
    } else {
      // The storage buffer variable IS the pointer - just record the mapping
      // We don't emit any instruction, just map the dest register to the
      // variable
      spirvIds[ir->destinations[ir_idx]] = ssbo_var_id;
    }
    break;
  }

  case IR::OP_STORAGE_FIELD: {
    // Access struct field in storage buffer: dest = ptr.field
    // operand0 = base pointer register
    // operand1 = field index (literal)
    // metadata = struct type hash
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 field_idx = ir->GetOperand(ir_idx, 1);
    u32 base_id = GetSpirvId(base_reg);
    u32 structTypeHash = ir->metadata[ir_idx];

    // Get the element type from destination register
    CoreType elemType = CoreType::FLOAT;
    u16 dest_reg = ir->destinations[ir_idx];
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      if (regType != CoreType::VOID && regType != CoreType::INVALID) {
        elemType = regType;
      }
    }

    // For storage buffer struct field access, we need a pointer to the field
    // type The field type was already created when declaring the struct type We
    // must use the SAME type ID to match SPIR-V's strict type checking

    // Look up the cached field type ID from when the struct was created
    u32 fieldTypeId = 0;
    if (structTypeHash != 0) {
      for (u32 i = 0; i < structTypeCount; i++) {
        if (structTypeHashes[i] == structTypeHash) {
          if (field_idx < MAX_FIELDS_PER_STRUCT) {
            fieldTypeId =
                structFieldTypeIds[i * MAX_FIELDS_PER_STRUCT + field_idx];
          }
          break;
        }
      }
    }

    // Fallback to element type if field type wasn't found
    if (fieldTypeId == 0) {
      fieldTypeId = GetTypeId(elemType);
    }

    spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
    if (ir->registerStorageInfo && base_reg < ir->registerCount) {
      if (ir->registerStorageInfo[base_reg] &
          IR::IRProgram::STORAGE_IS_SHARED) {
        storageClass = spv::StorageClassWorkgroup;
      }
    }
    u32 field_ptr_type = GetPointerTypeId(fieldTypeId, storageClass);

    // Check if base is a storage buffer variable (needs extra indices for
    // wrapper struct)
    bool isStorageBufferVar = false;
    for (u32 b = 0; b < 32; b++) {
      if (storageBufferIds[b] == base_id) {
        isStorageBufferVar = true;
        break;
      }
    }

    u32 field_id = GetIntConstantId(field_idx, false);
    u32 zero_id = GetIntConstantId(0, true);

    if (isStorageBufferVar) {
      // Storage buffer variable - need to access through Block wrapper and
      // runtime array Access pattern: buffer -> 0 (block member) -> 0 (first
      // array element) -> field_idx
      if (currentFunctionSize + 7 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpAccessChain;
      currentFunction[currentFunctionSize++] = field_ptr_type;
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = base_id;
      currentFunction[currentFunctionSize++] =
          zero_id; // Block wrapper member 0 (runtime array)
      currentFunction[currentFunctionSize++] =
          zero_id; // First element in runtime array
      currentFunction[currentFunctionSize++] = field_id; // Field within struct
    } else {
      // Intermediate pointer (already pointing into struct) - direct field
      // access
      if (currentFunctionSize + 5 > currentFunctionCapacity) {
        GrowCurrentFunction();
      }
      currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpAccessChain;
      currentFunction[currentFunctionSize++] = field_ptr_type;
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = base_id;
      currentFunction[currentFunctionSize++] = field_id; // Field within struct
    }

    // Track the storage class for this pointer register
    if (dest_reg < idCapacity) {
      storagePtrStorageClass[dest_reg] = static_cast<u32>(storageClass);
    }
    break;
  }

  case IR::OP_STORAGE_INDEX: {
    // Index into array in storage buffer: dest = ptr[index]
    // operand0 = base pointer register (points to array)
    // operand1 = index register
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 index_reg = ir->GetOperand(ir_idx, 1);
    u32 base_id = GetSpirvId(base_reg);
    u32 index_id = GetSpirvId(index_reg);

    // Get the element type from destination register
    CoreType elemType = CoreType::FLOAT;
    u16 dest_reg = ir->destinations[ir_idx];
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      // Exclude invalid, void, and resource types (BUFFER, CBUFFER) that aren't
      // valid element types
      if (regType != CoreType::VOID && regType != CoreType::INVALID &&
          regType != CoreType::BUFFER && regType != CoreType::CBUFFER) {
        elemType = regType;
      }
    }

    // Determine storage class - first check tracked storage class from previous
    // pointer ops
    spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
    if (base_reg < idCapacity && storagePtrStorageClass[base_reg] != 0) {
      storageClass =
          static_cast<spv::StorageClass>(storagePtrStorageClass[base_reg]);
    } else if (ir->registerStorageInfo && base_reg < ir->registerCount) {
      if (ir->registerStorageInfo[base_reg] &
          IR::IRProgram::STORAGE_IS_SHARED) {
        storageClass = spv::StorageClassWorkgroup;
      }
    }

    // Get element type ID (handle struct types specially)
    u32 elem_type_id = GetTypeId(elemType);
    u32 structHash = 0;

    // First check for shared memory arrays
    bool isShared = (storageClass == spv::StorageClassWorkgroup) ||
                    (ir->registerStorageInfo && base_reg < ir->registerCount &&
                     (ir->registerStorageInfo[base_reg] &
                      IR::IRProgram::STORAGE_IS_SHARED));

    if (isShared && ir->sharedTypes) {
      u32 storageInfo = ir->registerStorageInfo[base_reg];
      u32 sharedIndex = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
      if (sharedIndex < ir->sharedVarCount) {
        CoreType sharedType =
            static_cast<CoreType>(ir->sharedTypes[sharedIndex]);
        if (sharedType != CoreType::VOID && sharedType != CoreType::INVALID) {
          elemType = sharedType;
          elem_type_id = GetTypeId(elemType);
        }
      }
    } else if (ir->registerStorageInfo && base_reg < ir->registerCount) {
      // Check buffer element types for storage buffers
      u32 storageInfo = ir->registerStorageInfo[base_reg];
      if (storageInfo != 0) {
        u32 binding = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
        if (binding < 32) {
          structHash = ir->bufferElementStructTypes[binding];
          // Also check for primitive element type if no struct type
          if (structHash == 0) {
            CoreType discoveredType =
                static_cast<CoreType>(ir->bufferElementCoreTypes[binding]);
            if (discoveredType != CoreType::VOID &&
                discoveredType != CoreType::INVALID &&
                discoveredType != CoreType::CUSTOM) {
              elemType = discoveredType;
              elem_type_id = GetTypeId(elemType);
            }
          }
        }
      }
    }

    // If not found, look up from destination or base register
    if (structHash == 0 &&
        (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM)) {
      if (ir->registerStructTypes) {
        if (dest_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[dest_reg];
        }
        if (structHash == 0 && base_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[base_reg];
        }
      }
    }

    // Get struct type ID if we found a struct hash
    if (structHash != 0) {
      elem_type_id = GetStructTypeId(structHash);
      elemType = CoreType::CUSTOM;
    }

    if (elem_type_id == 0) {
      elem_type_id = GetTypeId(CoreType::FLOAT);
    }

    // Get pointer type for the element
    u32 elem_ptr_type = GetPointerTypeId(elem_type_id, storageClass);

    // Check if base is a storage buffer or uniform buffer variable (needs
    // extra 0 index for the wrapper struct). Skip this check for shared
    // memory - shared arrays don't have wrapper structs
    bool isBufferVar = false;
    if (!isShared) {
      for (u32 b = 0; b < 32; b++) {
        if (storageBufferIds[b] == base_id || uniformBufferIds[b] == base_id) {
          isBufferVar = true;
          break;
        }
      }
    }

    if (isBufferVar) {
      // Buffer variable - need to access member 0 (the array) first
      u32 zero_id = GetIntConstantId(0, true);
      Emit(spv::OpAccessChain, elem_ptr_type, dest, base_id, zero_id, index_id);
    } else {
      // Intermediate pointer - direct index
      Emit(spv::OpAccessChain, elem_ptr_type, dest, base_id, index_id);
    }

    // Store element type and storage class for this pointer register
    if (dest_reg < idCapacity) {
      storagePtrElemTypes[dest_reg] = elem_type_id;
      storagePtrStorageClass[dest_reg] = static_cast<u32>(storageClass);
    }
    break;
  }

  case IR::OP_STORAGE_LOAD: {
    // Load value from storage buffer pointer: dest = *ptr
    // operand0 = pointer register
    u16 ptr_reg = ir->GetOperand(ir_idx, 0);
    u32 ptr_id = GetSpirvId(ptr_reg);

    // Get the element type from destination register
    CoreType elemType = CoreType::FLOAT;
    u16 dest_reg = ir->destinations[ir_idx];
    if (ir->registerTypes && dest_reg < ir->registerCount) {
      CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
      // Exclude invalid, void, and resource types (BUFFER, CBUFFER) that aren't
      // valid element types
      if (regType != CoreType::VOID && regType != CoreType::INVALID &&
          regType != CoreType::BUFFER && regType != CoreType::CBUFFER) {
        elemType = regType;
      }
    }

    // Get element type ID (handle struct types specially)
    u32 elem_type_id = GetTypeId(elemType);
    u32 structHash = 0;

    // First check for shared memory arrays
    bool isShared =
        ir->registerStorageInfo && ptr_reg < ir->registerCount &&
        (ir->registerStorageInfo[ptr_reg] & IR::IRProgram::STORAGE_IS_SHARED);

    if (isShared && ir->sharedTypes) {
      u32 storageInfo = ir->registerStorageInfo[ptr_reg];
      u32 sharedIndex = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
      if (sharedIndex < ir->sharedVarCount) {
        CoreType sharedType =
            static_cast<CoreType>(ir->sharedTypes[sharedIndex]);
        if (sharedType != CoreType::VOID && sharedType != CoreType::INVALID) {
          elemType = sharedType;
          elem_type_id = GetTypeId(elemType);
        }
      }
    } else if (ir->registerStorageInfo && ptr_reg < ir->registerCount) {
      // Check buffer element types for storage buffers
      u32 storageInfo = ir->registerStorageInfo[ptr_reg];
      if (storageInfo != 0) {
        u32 binding = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
        if (binding < 32) {
          structHash = ir->bufferElementStructTypes[binding];
          // Also check for primitive element type if no struct type
          if (structHash == 0) {
            CoreType discoveredType =
                static_cast<CoreType>(ir->bufferElementCoreTypes[binding]);
            if (discoveredType != CoreType::VOID &&
                discoveredType != CoreType::INVALID &&
                discoveredType != CoreType::CUSTOM) {
              elemType = discoveredType;
              elem_type_id = GetTypeId(elemType);
            }
          }
        }
      }
    }

    // If not found, look up from destination or pointer register
    if (structHash == 0 &&
        (elemType == CoreType::CUSTOM || elemType == CoreType::ENUM)) {
      if (ir->registerStructTypes) {
        if (dest_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[dest_reg];
        }
        if (structHash == 0 && ptr_reg < ir->registerCount) {
          structHash = ir->registerStructTypes[ptr_reg];
        }
      }
    }

    // Get struct type ID if we found a struct hash
    if (structHash != 0) {
      elem_type_id = GetStructTypeId(structHash);
    }

    // Fallback: use the element type stored by STORAGE_INDEX
    if (elem_type_id == 0 && ptr_reg < idCapacity &&
        storagePtrElemTypes[ptr_reg] != 0) {
      elem_type_id = storagePtrElemTypes[ptr_reg];
    }

    if (elem_type_id == 0) {
      elem_type_id = GetTypeId(CoreType::FLOAT);
    }

    // Emit OpLoad
    Emit(spv::OpLoad, elem_type_id, dest, ptr_id);
    break;
  }

  case IR::OP_BRANCH: {
    u16 cond_reg = ir->GetOperand(ir_idx, 0);
    u32 true_target = ir->GetBranchTrueTarget(ir_idx);
    u32 false_target = ir->GetBranchFalseTarget(ir_idx);

    // Check if EmitBranch already converted this condition to bool
    u32 condition;
    if (branchConditionOverride != 0 &&
        branchConditionOverrideReg == cond_reg) {
      // Use the pre-converted bool
      condition = branchConditionOverride;
    } else {
      condition = GetSpirvId(cond_reg);

      // SPIR-V requires boolean condition for OpBranchConditional
      // If condition is not bool, convert it to bool via != 0 comparison
      CoreType condType = CoreType::BOOL;
      if (cond_reg & 0xC000) {
        // Constant-encoded condition
        if ((cond_reg & 0xC000) == 0xC000) {
          condType = CoreType::BOOL;
        } else if (cond_reg & 0x8000) {
          condType = CoreType::FLOAT;
        } else if (cond_reg & 0x4000) {
          condType = CoreType::INT;
        } else if (cond_reg & 0x2000) {
          condType = CoreType::UINT;
        }
      } else if (cond_reg < ir->registerCount && ir->registerTypes) {
        condType = static_cast<CoreType>(ir->registerTypes[cond_reg]);
      }

      if (condType != CoreType::BOOL) {
        // Convert non-bool to bool: condition != 0
        u32 bool_type = GetTypeId(CoreType::BOOL);
        u32 bool_result = AllocateId();

        // Determine comparison op and zero constant based on type
        if (mask(condType) & TypeMasks::FLOAT_TYPES) {
          u32 zero = GetFloatConstantId(0.0f);
          Emit(spv::OpFOrdNotEqual, bool_type, bool_result, condition, zero);
        } else if (mask(condType) & TypeMasks::UINT_TYPES) {
          u32 zero = GetIntConstantId(0, true); // unsigned zero
          Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
        } else if (mask(condType) & TypeMasks::INT_TYPES) {
          u32 zero = GetIntConstantId(0, false); // signed zero
          Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
        } else {
          // Default: treat as int comparison
          u32 zero = GetIntConstantId(0, false);
          Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
        }
        condition = bool_result;
      }
    }

    u32 true_label = GetOrCreateBlockLabel(true_target);
    u32 false_label = GetOrCreateBlockLabel(false_target);
    Emit(spv::OpBranchConditional, condition, true_label, false_label);
    break;
  }

  case IR::OP_JUMP: {
    u32 target = ir->metadata[ir_idx]; // Target instruction index in metadata
    u32 label = GetOrCreateBlockLabel(target);
    Emit(spv::OpBranch, label);
    break;
  }

  case IR::OP_SWITCH: {
    // Get switch data from IR - metadata stores the switchId
    u32 switchId = ir->metadata[ir_idx];
    u32 selector = GetSpirvId(ir->GetOperand(ir_idx, 0));
    u32 default_target = ir->GetSwitchDefaultTarget(switchId);
    u32 default_label = GetOrCreateBlockLabel(default_target);

    // Get case count
    u32 caseCount = ir->GetSwitchCaseCount(switchId);

    // Build OpSwitch: selector, default_label, then pairs of (literal, label)
    // Total words: 3 + 2*caseCount
    if (currentFunctionSize + 3 + 2 * caseCount > currentFunctionCapacity) {
      GrowCurrentFunction();
    }

    currentFunction[currentFunctionSize++] =
        ((3 + 2 * caseCount) << 16) | spv::OpSwitch;
    currentFunction[currentFunctionSize++] = selector;
    currentFunction[currentFunctionSize++] = default_label;

    for (u32 i = 0; i < caseCount; i++) {
      u32 caseValue = ir->GetSwitchCaseValue(switchId, i);
      u32 caseTarget = ir->GetSwitchCaseTarget(switchId, i);
      u32 caseLabel = GetOrCreateBlockLabel(caseTarget);

      currentFunction[currentFunctionSize++] = caseValue;
      currentFunction[currentFunctionSize++] = caseLabel;
    }
    break;
  }

  case IR::OP_RET: {
    if (ir->destinations[ir_idx] != 0) {
      u32 value = GetSpirvId(ir->destinations[ir_idx]);
      Emit(spv::OpReturnValue, value);
    } else {
      Emit(spv::OpReturn);
    }
    break;
  }

  case IR::OP_DISCARD: {
    // Fragment discard - terminates fragment shader execution
    // OpKill must appear in a fragment shader only
    Emit(spv::OpKill);
    break;
  }

  // ========== Local Pointer Operations ==========
  case IR::OP_LOCAL_VAR_PTR: {
    // Get pointer to local variable
    // In SPIR-V, we need an OpVariable to have a pointer
    // The OpVariable was pre-allocated and emitted at function start
    u16 var_reg = ir->GetOperand(ir_idx, 0);
    u16 dest_reg = ir->destinations[ir_idx];

    // Get the pre-allocated OpVariable for this register
    u32 var_id = 0;
    if (var_reg < idCapacity) {
      var_id = localVarIds[var_reg];
    }

    if (var_id != 0) {
      // Store the current value into the pre-allocated OpVariable
      u32 src_id = GetSpirvId(var_reg);
      Emit(spv::OpStore, var_id, src_id);
    } else {
      // Should not happen if pre-pass worked correctly
      fprintf(stderr,
              "Warning: OP_LOCAL_VAR_PTR without pre-allocated variable for "
              "reg %u\n",
              var_reg);
    }

    // The result of address-of is the OpVariable itself (which is a pointer)
    spirvIds[dest_reg] = var_id;
    break;
  }

  case IR::OP_LOCAL_LOAD: {
    // Load from local pointer - OpLoad
    // The pointer register's storageInfo contains the source variable register
    u16 ptr_reg = ir->GetOperand(ir_idx, 0);
    u32 storageInfo = 0;
    if (ir->registerStorageInfo && ptr_reg < ir->registerCount) {
      storageInfo = ir->registerStorageInfo[ptr_reg];
    }
    bool isFieldPtr = (storageInfo & IR::IRProgram::STORAGE_IS_FIELD_PTR) != 0;
    // Extract source register (bits 16-31) and pointee type (bits 8-15)
    u16 src_reg = static_cast<u16>((storageInfo >> 16) & 0xFFFF);
    CoreType pointeeType = static_cast<CoreType>((storageInfo >> 8) & 0xFF);
    if (pointeeType == CoreType::INVALID || pointeeType == CoreType::VOID) {
      pointeeType = CoreType::FLOAT; // Fallback
    }
    u32 var_id = 0;
    if (isFieldPtr) {
      // Field pointers route through the access chain we stored in
      // spirvIds[ptr_reg]. The base struct's OpVariable still needs an
      // OpStore at the ADDRESS_OF site to keep memory coherent with
      // register-level struct writes (handled by OP_LOCAL_FIELD_PTR).
      var_id = GetSpirvId(ptr_reg);
    } else {
      // Get the OpVariable for the source register
      if (src_reg < idCapacity) {
        var_id = localVarIds[src_reg];
      }
      if (var_id == 0) {
        // Fallback: use the pointer register's SPIR-V ID
        var_id = GetSpirvId(ptr_reg);
      }
    }
    u32 result_type_id = GetTypeId(pointeeType);
    // For struct pointees, resolve the struct type id from the hash stored on
    // the pointer register. Without this, loading from a struct pointer emits
    // OpLoad with result type 0 and fails SPIR-V validation.
    if (pointeeType == CoreType::CUSTOM && ir->registerStructTypes &&
        ptr_reg < ir->registerCount) {
      u32 structHash = ir->registerStructTypes[ptr_reg];
      if (structHash != 0) {
        result_type_id = GetStructTypeId(structHash);
      }
    }
    Emit(spv::OpLoad, result_type_id, dest, var_id);
    // Track the loaded register's struct type so subsequent OpCompositeExtract
    // on it emits with the correct member type.
    if (pointeeType == CoreType::CUSTOM && ir->registerStructTypes &&
        ptr_reg < ir->registerCount && dest < idCapacity) {
      u32 structHash = ir->registerStructTypes[ptr_reg];
      if (structHash != 0) {
        // Propagation happens at IR level already, but make sure the loaded
        // register's type is propagated for backends that rely on it.
      }
    }
    break;
  }

  case IR::OP_LOCAL_STORE: {
    // Store to local pointer - OpStore
    // Format: dest=0, operand0=ptr, operand1=value
    // The pointer register's storageInfo contains the source variable register
    u16 ptr_reg = ir->GetOperand(ir_idx, 0);
    u16 val_reg = ir->GetOperand(ir_idx, 1);

    u32 storageInfo = 0;
    if (ir->registerStorageInfo && ptr_reg < ir->registerCount) {
      storageInfo = ir->registerStorageInfo[ptr_reg];
    }
    bool isFieldPtr = (storageInfo & IR::IRProgram::STORAGE_IS_FIELD_PTR) != 0;
    // Extract source register (bits 16-31)
    u16 src_reg = static_cast<u16>((storageInfo >> 16) & 0xFFFF);
    u32 var_id = 0;
    if (isFieldPtr) {
      var_id = GetSpirvId(ptr_reg);
    } else {
      // Get the OpVariable for the source register
      if (src_reg < idCapacity) {
        var_id = localVarIds[src_reg];
      }
      if (var_id == 0) {
        // Fallback: use the pointer register's SPIR-V ID
        var_id = GetSpirvId(ptr_reg);
      }
    }
    u32 val_id = GetSpirvId(val_reg);
    Emit(spv::OpStore, var_id, val_id);
    break;
  }

  case IR::OP_LOCAL_FIELD_PTR: {
    // dest = &base.field
    // operand0 = base struct variable register
    // operand1 = field index (literal)
    // metadata = struct type hash
    u16 base_reg = ir->GetOperand(ir_idx, 0);
    u16 field_idx = ir->GetOperand(ir_idx, 1);
    u16 dest_reg = ir->destinations[ir_idx];
    u32 struct_type_hash = ir->metadata[ir_idx];

    // Ensure the base struct OpVariable holds the current SSA value so the
    // subsequent access chain sees the same data that register code sees.
    u32 var_id = (base_reg < idCapacity) ? localVarIds[base_reg] : 0;
    if (var_id == 0) {
      fprintf(stderr,
              "Error: OP_LOCAL_FIELD_PTR without pre-allocated variable for "
              "base reg %u\n",
              base_reg);
      break;
    }
    u32 base_val_id = GetSpirvId(base_reg);
    if (base_val_id != 0) {
      Emit(spv::OpStore, var_id, base_val_id);
    }

    // Resolve the field's SPIR-V type.
    CoreType fieldType = CoreType::FLOAT;
    u32 fieldStructHash = 0;
    if (struct_type_hash != 0 && ir->structTypes) {
      for (u32 i = 0; i < ir->structTypeCount; i++) {
        if (ir->structTypes[i].nameHash == struct_type_hash) {
          const IR::IRProgram::StructTypeInfo &info = ir->structTypes[i];
          if (field_idx < info.fieldCount) {
            fieldType = static_cast<CoreType>(
                ir->structFieldTypes[info.fieldOffset + field_idx]);
            if (ir->structFieldTypeHashes) {
              fieldStructHash =
                  ir->structFieldTypeHashes[info.fieldOffset + field_idx];
            }
          }
          break;
        }
      }
    }
    u32 field_type_id = 0;
    if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
        fieldStructHash != 0) {
      field_type_id = GetStructTypeId(fieldStructHash);
    }
    if (field_type_id == 0) {
      field_type_id = GetTypeId(fieldType);
    }
    if (field_type_id == 0) {
      field_type_id = GetTypeId(CoreType::FLOAT);
    }
    u32 ptr_type_id =
        GetPointerTypeId(field_type_id, spv::StorageClassFunction);
    u32 field_const_id = GetIntConstantId(field_idx, false);

    Emit(spv::OpAccessChain, ptr_type_id, dest, var_id, field_const_id);
    if (dest_reg < idCapacity) {
      spirvIds[dest_reg] = dest;
    }
    break;
  }

  case IR::OP_BARRIER: {
    u32 scope = GetIntConstantId(static_cast<u32>(spv::ScopeWorkgroup), true);
    u32 semantics = GetIntConstantId(
        static_cast<u32>(spv::MemorySemanticsAcquireReleaseMask |
                         spv::MemorySemanticsWorkgroupMemoryMask),
        true);
    Emit(spv::OpControlBarrier, scope, scope, semantics);
    break;
  }

  case IR::OP_MEM_FENCE: {
    u32 scope = GetIntConstantId(static_cast<u32>(spv::ScopeWorkgroup), true);
    u32 semantics = GetIntConstantId(
        static_cast<u32>(spv::MemorySemanticsAcquireReleaseMask |
                         spv::MemorySemanticsUniformMemoryMask |
                         spv::MemorySemanticsWorkgroupMemoryMask),
        true);
    Emit(spv::OpMemoryBarrier, scope, semantics);
    break;
  }

  case IR::OP_WAVE_SUM:
  case IR::OP_WAVE_MUL:
  case IR::OP_WAVE_MIN:
  case IR::OP_WAVE_MAX:
  case IR::OP_WAVE_ALL:
  case IR::OP_WAVE_ANY:
  case IR::OP_WAVE_READ_LANE:
  case IR::OP_WAVE_READ_FIRST: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 value_reg = ir->GetOperand(ir_idx, 0);
    u32 value_id = GetSpirvId(value_reg);

    u32 result_type = GetResultType(dest_reg, value_reg);
    u32 scope = GetIntConstantId(static_cast<u32>(spv::ScopeSubgroup), true);

    if (op == IR::OP_WAVE_ALL || op == IR::OP_WAVE_ANY) {
      spv::Op boolOp = (op == IR::OP_WAVE_ALL) ? static_cast<spv::Op>(334)
                                               : static_cast<spv::Op>(335);
      Emit(boolOp, result_type, dest, scope, value_id);
      break;
    }

    auto get_uint_constant_lane_id = [&](u16 lane_reg) -> u32 {
      if ((lane_reg & 0xC000) == 0xC000) {
        u32 idx = lane_reg & 0x3FFF;
        if (idx >= ir->boolCount) {
          return 0;
        }
        return GetIntConstantId(ir->boolConstants[idx] != 0 ? 1u : 0u, true);
      }
      if (lane_reg & 0x4000) {
        u32 idx = lane_reg & 0x3FFF;
        if (idx >= ir->intCount) {
          return 0;
        }
        return GetIntConstantId(ir->intConstants[idx], true);
      }
      if (lane_reg & 0x2000) {
        return GetSpirvId(lane_reg);
      }
      if (lane_reg & 0x8000) {
        return 0;
      }

      if (lane_reg >= idCapacity) {
        return 0;
      }

      u32 lane_id = spirvIds[lane_reg];
      if (lane_id == 0) {
        return 0;
      }
      if (lane_id == boolTrueId) {
        return GetIntConstantId(1, true);
      }
      if (lane_id == boolFalseId) {
        return GetIntConstantId(0, true);
      }
      for (u32 i = 0; i < ir->intCount; i++) {
        if (uintConstantIds[i] == lane_id) {
          return lane_id;
        }
        if (intConstantIds[i] == lane_id) {
          return GetIntConstantId(ir->intConstants[i], true);
        }
      }
      return 0;
    };

    auto get_uint_lane_id = [&](u16 lane_reg) -> u32 {
      if (u32 constant_lane_id = get_uint_constant_lane_id(lane_reg)) {
        return constant_lane_id;
      }

      u32 lane_id = GetSpirvId(lane_reg);
      CoreType laneType = GetOperandType(lane_reg);
      if (laneType == CoreType::UINT) {
        return lane_id;
      }

      u32 uint_type = GetTypeId(CoreType::UINT);
      u32 converted_lane = AllocateId();
      switch (laneType) {
      case CoreType::INT:
        Emit(spv::OpBitcast, uint_type, converted_lane, lane_id);
        break;
      case CoreType::FLOAT:
        Emit(spv::OpConvertFToU, uint_type, converted_lane, lane_id);
        break;
      case CoreType::BOOL: {
        u32 one = GetIntConstantId(1, true);
        u32 zero = GetIntConstantId(0, true);
        Emit(spv::OpSelect, uint_type, converted_lane, lane_id, one, zero);
        break;
      }
      default:
        Emit(spv::OpBitcast, uint_type, converted_lane, lane_id);
        break;
      }
      return converted_lane;
    };

    if (op == IR::OP_WAVE_READ_FIRST) {
      Emit(static_cast<spv::Op>(338), result_type, dest, scope, value_id);
      break;
    }

    if (op == IR::OP_WAVE_READ_LANE) {
      u16 lane_reg = ir->GetOperand(ir_idx, 1);
      if (u32 constant_lane_id = get_uint_constant_lane_id(lane_reg)) {
        Emit(static_cast<spv::Op>(337), result_type, dest, scope, value_id,
             constant_lane_id);
      } else {
        u32 lane_id = get_uint_lane_id(lane_reg);
        Emit(static_cast<spv::Op>(345), result_type, dest, scope, value_id,
             lane_id);
      }
      break;
    }

    CoreType valueType = GetOperandType(value_reg);
    bool isFloat = (mask(valueType) & TypeMasks::FLOAT_TYPES) != 0;
    bool isUint = (valueType == CoreType::UINT);

    u32 groupOp = static_cast<u32>(spv::GroupOperationReduce);
    spv::Op groupOpCode = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];

    if (op == IR::OP_WAVE_SUM) {
      groupOpCode =
          isFloat ? static_cast<spv::Op>(350) : static_cast<spv::Op>(349);
    } else if (op == IR::OP_WAVE_MUL) {
      groupOpCode =
          isFloat ? static_cast<spv::Op>(352) : static_cast<spv::Op>(351);
    } else if (op == IR::OP_WAVE_MIN) {
      if (isFloat) {
        groupOpCode = static_cast<spv::Op>(355);
      } else {
        groupOpCode =
            isUint ? static_cast<spv::Op>(354) : static_cast<spv::Op>(353);
      }
    } else if (op == IR::OP_WAVE_MAX) {
      if (isFloat) {
        groupOpCode = static_cast<spv::Op>(358);
      } else {
        groupOpCode =
            isUint ? static_cast<spv::Op>(357) : static_cast<spv::Op>(356);
      }
    }

    Emit(groupOpCode, result_type, dest, scope, groupOp, value_id);
    break;
  }

  case IR::OP_ATOMIC_ADD:
  case IR::OP_ATOMIC_MIN:
  case IR::OP_ATOMIC_MAX:
  case IR::OP_ATOMIC_AND:
  case IR::OP_ATOMIC_OR:
  case IR::OP_ATOMIC_XOR:
  case IR::OP_ATOMIC_XCHG:
  case IR::OP_ATOMIC_CMP_XCHG: {
    u16 dest_reg = ir->destinations[ir_idx];
    u16 ptr_reg = ir->GetOperand(ir_idx, 0);
    u16 value_reg = ir->GetOperand(ir_idx, 1);
    u16 cmp_reg = ir->GetOperand(ir_idx, 2);

    u32 ptr_id = GetSpirvId(ptr_reg);
    u32 value_id = GetSpirvId(value_reg);

    u32 result_type = GetResultType(dest_reg, value_reg);

    bool isShared = false;
    if (ir->registerStorageInfo && ptr_reg < ir->registerCount) {
      isShared = (ir->registerStorageInfo[ptr_reg] &
                  IR::IRProgram::STORAGE_IS_SHARED) != 0;
    }

    u32 scope = GetIntConstantId(
        static_cast<u32>(isShared ? spv::ScopeWorkgroup : spv::ScopeDevice),
        true);
    u32 semanticsMask = spv::MemorySemanticsAcquireReleaseMask |
                        (isShared ? spv::MemorySemanticsWorkgroupMemoryMask
                                  : spv::MemorySemanticsUniformMemoryMask);
    u32 semantics = GetIntConstantId(static_cast<u32>(semanticsMask), true);

    spv::Op atomicOp = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
    if (op == IR::OP_ATOMIC_MIN || op == IR::OP_ATOMIC_MAX) {
      CoreType valueType = GetOperandType(value_reg);
      bool useUnsigned = (valueType == CoreType::UINT);
      if (op == IR::OP_ATOMIC_MIN) {
        atomicOp = useUnsigned ? spv::OpAtomicUMin : spv::OpAtomicSMin;
      } else {
        atomicOp = useUnsigned ? spv::OpAtomicUMax : spv::OpAtomicSMax;
      }
    }

    if (op == IR::OP_ATOMIC_CMP_XCHG) {
      u32 cmp_id = GetSpirvId(cmp_reg);
      u32 failureMask = (isShared ? spv::MemorySemanticsWorkgroupMemoryMask
                                  : spv::MemorySemanticsUniformMemoryMask) |
                        spv::MemorySemanticsAcquireMask;
      u32 failureSemantics =
          GetIntConstantId(static_cast<u32>(failureMask), true);
      Emit(spv::OpAtomicCompareExchange, result_type, dest, ptr_id, scope,
           semantics, failureSemantics, value_id, cmp_id);
    } else {
      Emit(atomicOp, result_type, dest, ptr_id, scope, semantics, value_id);
    }
    break;
  }

  case IR::OP_TEX_SAMPLE:
  case IR::OP_TEX_SAMPLE_LOD:
  case IR::OP_TEX_SAMPLE_BIAS:
  case IR::OP_TEX_SAMPLE_GRAD:
  case IR::OP_TEX_SAMPLE_CMP:
  case IR::OP_TEX_SAMPLE_OFFSET:
  case IR::OP_TEX_SAMPLE_LOD_OFFSET:
  case IR::OP_TEX_SAMPLE_BIAS_OFFSET: {
    // Texture sampling: dest = sample(texture, coord)
    // IR format: s0 = texture (with 0x2000 marker), s1 = coord
    // The resources are declared as combined image samplers
    // (OpTypeSampledImage)
    u16 tex_reg = ir->GetOperand(ir_idx, 0);
    u16 coord_reg = ir->GetOperand(ir_idx, 1);
    u32 coord_id = GetSpirvId(coord_reg);

    // Get result type (float4 for most texture samples)
    u32 result_type = GetTypeId(CoreType::FLOAT4);
    SampledTextureLoad texture{};
    if (!LoadSampledTexture(tex_reg, CoreType::FLOAT4, dest, false, &texture)) {
      break;
    }

    auto emitImageInst = [&](spv::Op imageOp, u32 resultType, u32 imageOperands,
                             const u32 *extraOperands, u32 extraCount) {
      u32 wordCount = 5 + (imageOperands != 0 ? 1 + extraCount : 0);
      if (currentFunctionSize + wordCount > currentFunctionCapacity)
        GrowCurrentFunction();
      currentFunction[currentFunctionSize++] = (wordCount << 16) | imageOp;
      currentFunction[currentFunctionSize++] = resultType;
      currentFunction[currentFunctionSize++] = dest;
      currentFunction[currentFunctionSize++] = texture.sampledImageId;
      currentFunction[currentFunctionSize++] = coord_id;
      if (imageOperands != 0) {
        currentFunction[currentFunctionSize++] = imageOperands;
        for (u32 i = 0; i < extraCount; i++) {
          currentFunction[currentFunctionSize++] = extraOperands[i];
        }
      }
    };

    // Sample the texture
    switch (op) {
    case IR::OP_TEX_SAMPLE:
      emitImageInst(spv::OpImageSampleImplicitLod, result_type, 0, nullptr, 0);
      break;
    case IR::OP_TEX_SAMPLE_OFFSET: {
      u32 offset_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 extras[1] = {offset_id};
      emitImageInst(spv::OpImageSampleImplicitLod, result_type,
                    spv::ImageOperandsOffsetMask, extras, 1);
      break;
    }
    case IR::OP_TEX_SAMPLE_LOD: {
      u32 lod_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 extras[1] = {lod_id};
      emitImageInst(spv::OpImageSampleExplicitLod, result_type,
                    spv::ImageOperandsLodMask, extras, 1);
      break;
    }
    case IR::OP_TEX_SAMPLE_LOD_OFFSET: {
      u32 lod_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 offset_id = GetSpirvId(ir->GetOperand(ir_idx, 3));
      u32 extras[2] = {lod_id, offset_id};
      emitImageInst(spv::OpImageSampleExplicitLod, result_type,
                    spv::ImageOperandsLodMask | spv::ImageOperandsOffsetMask,
                    extras, 2);
      break;
    }
    case IR::OP_TEX_SAMPLE_BIAS: {
      u32 bias_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 extras[1] = {bias_id};
      emitImageInst(spv::OpImageSampleImplicitLod, result_type,
                    spv::ImageOperandsBiasMask, extras, 1);
      break;
    }
    case IR::OP_TEX_SAMPLE_BIAS_OFFSET: {
      u32 bias_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 offset_id = GetSpirvId(ir->GetOperand(ir_idx, 3));
      u32 extras[2] = {bias_id, offset_id};
      emitImageInst(spv::OpImageSampleImplicitLod, result_type,
                    spv::ImageOperandsBiasMask | spv::ImageOperandsOffsetMask,
                    extras, 2);
      break;
    }
    case IR::OP_TEX_SAMPLE_GRAD: {
      // Explicit gradients - operand 2 is ddx, operand 3 is ddy
      u32 ddx_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 ddy_id = GetSpirvId(ir->GetOperand(ir_idx, 3));
      u32 extras[2] = {ddx_id, ddy_id};
      emitImageInst(spv::OpImageSampleExplicitLod, result_type,
                    spv::ImageOperandsGradMask, extras, 2);
      break;
    }
    case IR::OP_TEX_SAMPLE_CMP: {
      // Depth-comparison sample - operand 2 is the reference value.
      // OpImageSampleDrefImplicitLod produces a scalar float, while the IR
      // contract types every texture sample as FLOAT4, so splat the
      // comparison result into the destination register.
      u32 dref_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
      u32 cmp_id = AllocateId();
      Emit(spv::OpImageSampleDrefImplicitLod, GetTypeId(CoreType::FLOAT),
           cmp_id, texture.sampledImageId, coord_id, dref_id);
      Emit(spv::OpCompositeConstruct, result_type, dest, cmp_id, cmp_id,
           cmp_id, cmp_id);
      break;
    }
    default:
      break;
    }
    break;
  }

  case IR::OP_TEX_FETCH:
  case IR::OP_TEX_FETCH_OFFSET: {
    u16 tex_reg = ir->GetOperand(ir_idx, 0);
    u16 coord_reg = ir->GetOperand(ir_idx, 1);
    u16 lod_reg = ir->GetOperand(ir_idx, 2);
    SampledTextureLoad texture{};
    if (!LoadSampledTexture(tex_reg, CoreType::FLOAT4, dest, true, &texture)) {
      break;
    }

    u32 result_type = GetTypeId(CoreType::FLOAT4);
    u32 coord_id = GetSpirvId(coord_reg);
    u32 lod_id = GetSpirvId(lod_reg);
    u32 imageOperands = spv::ImageOperandsLodMask;
    u32 extras[2] = {lod_id, 0};
    u32 extraCount = 1;
    if (op == IR::OP_TEX_FETCH_OFFSET) {
      extras[1] = GetSpirvId(ir->GetOperand(ir_idx, 3));
      imageOperands |= spv::ImageOperandsOffsetMask;
      extraCount = 2;
    }

    u32 wordCount = 5 + 1 + extraCount;
    if (currentFunctionSize + wordCount > currentFunctionCapacity)
      GrowCurrentFunction();
    currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpImageFetch;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = texture.imageId;
    currentFunction[currentFunctionSize++] = coord_id;
    currentFunction[currentFunctionSize++] = imageOperands;
    for (u32 i = 0; i < extraCount; i++) {
      currentFunction[currentFunctionSize++] = extras[i];
    }
    break;
  }

  case IR::OP_TEX_GATHER:
  case IR::OP_TEX_GATHER_OFFSET: {
    u16 tex_reg = ir->GetOperand(ir_idx, 0);
    u16 coord_reg = ir->GetOperand(ir_idx, 1);
    u16 component_reg = ir->GetOperand(ir_idx, 2);
    u32 coord_id = GetSpirvId(coord_reg);

    SampledTextureLoad texture{};
    if (!LoadSampledTexture(tex_reg, CoreType::FLOAT4, dest, false, &texture)) {
      break;
    }

    u32 result_type = GetTypeId(CoreType::FLOAT4);
    u32 component_id = GetSpirvId(component_reg);
    u32 imageOperands = 0;
    u32 offset_id = 0;
    u32 extraCount = 0;
    if (op == IR::OP_TEX_GATHER_OFFSET) {
      imageOperands = spv::ImageOperandsOffsetMask;
      offset_id = GetSpirvId(ir->GetOperand(ir_idx, 3));
      extraCount = 1;
    }

    u32 wordCount = 6 + (imageOperands != 0 ? 1 + extraCount : 0);
    if (currentFunctionSize + wordCount > currentFunctionCapacity)
      GrowCurrentFunction();
    currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpImageGather;
    currentFunction[currentFunctionSize++] = result_type;
    currentFunction[currentFunctionSize++] = dest;
    currentFunction[currentFunctionSize++] = texture.sampledImageId;
    currentFunction[currentFunctionSize++] = coord_id;
    currentFunction[currentFunctionSize++] = component_id;
    if (imageOperands != 0) {
      currentFunction[currentFunctionSize++] = imageOperands;
      currentFunction[currentFunctionSize++] = offset_id;
    }
    break;
  }

  case IR::OP_TEX_SIZE: {
    u16 tex_reg = ir->GetOperand(ir_idx, 0);
    u16 lod_reg = ir->GetOperand(ir_idx, 1);
    SampledTextureLoad texture{};
    if (!LoadSampledTexture(tex_reg, CoreType::INT2, dest, true, &texture)) {
      break;
    }

    u32 result_type = GetResultType(ir->destinations[ir_idx], tex_reg);
    u32 lod_id = GetSpirvId(lod_reg);
    Emit(spv::OpImageQuerySizeLod, result_type, dest, texture.imageId, lod_id);
    break;
  }

  case IR::OP_TEX_LEVELS: {
    u16 tex_reg = ir->GetOperand(ir_idx, 0);
    SampledTextureLoad texture{};
    if (!LoadSampledTexture(tex_reg, CoreType::INT, dest, true, &texture)) {
      break;
    }

    u32 result_type = GetTypeId(CoreType::INT);
    Emit(spv::OpImageQueryLevels, result_type, dest, texture.imageId);
    break;
  }

  case IR::OP_IMG_STORE: {
    // Image store: store(image, coord, value)
    // IR format: s0 = image (with 0x2000 marker), s1 = coord (int2), s2 = value
    // (float4) OpImageWrite has no result - it's a void operation
    u16 img_reg = ir->GetOperand(ir_idx, 0);
    u16 coord_reg = ir->GetOperand(ir_idx, 1);
    u16 value_reg = ir->GetOperand(ir_idx, 2);

    u16 img_slot = img_reg & 0x0FFF; // Extract binding from 0x2000 | binding

    // Get the storage image variable ID from the dedicated storage image array
    u32 img_var_id = storageImageIds[img_slot];

    if (img_var_id == 0) {
      // Storage image not declared - this shouldn't happen if analysis worked
      // correctly
      fprintf(stderr, "Error: Storage image at binding %u not declared\n",
              img_slot);
      break;
    }

    // Get coordinate and value SPIR-V IDs
    u32 coord_id = GetSpirvId(coord_reg);
    u32 value_id = GetSpirvId(value_reg);

    // Get storage image type (should already be created during
    // DeclareResources)
    u32 storage_img_type = GetStorageImageTypeId();

    // Load the image from the variable
    u32 img_id = AllocateId();
    Emit(spv::OpLoad, storage_img_type, img_id, img_var_id);

    // Emit OpImageWrite: Image, Coordinate, Texel
    // OpImageWrite has no result, word count = 4
    if (currentFunctionSize + 4 > currentFunctionCapacity)
      GrowCurrentFunction();
    currentFunction[currentFunctionSize++] = (4 << 16) | spv::OpImageWrite;
    currentFunction[currentFunctionSize++] = img_id;
    currentFunction[currentFunctionSize++] = coord_id;
    currentFunction[currentFunctionSize++] = value_id;
    break;
  }

  // These opcodes are handled in other backend phases, share a numeric value
  // with another case, or are not emitted by the current SPIR-V path.
  case IR::OP_PHI:
  case IR::OP_LOAD_BUFFER:
  case IR::OP_STORE_BUFFER:
  case IR::OP_LOAD_LOCAL:
  case IR::OP_STORE_LOCAL:
  case IR::OP_LOAD_SHARED:
  case IR::OP_STORE_SHARED:
  case IR::OP_F2F16:
  case IR::OP_F162F:
  case IR::OP_STRUCT_LOAD:
  case IR::OP_STRUCT_STORE:
  case IR::OP_STRUCT_GEP:
  case IR::OP_MAT_IDENTITY:
  case IR::OP_MAT_ZERO:
  case IR::OP_IMG_LOAD:
  case IR::OP_LOAD_TEX_HANDLE:
  case IR::OP_ATOMIC_SUB:
  case IR::OP_WAVE_BALLOT:
  case IR::OP_ARRAY_ACCESS:
  case IR::OP_ARRAY_CONSTRUCT:
  case IR::OP_INVALID:
    break;

    // TODO: Add more opcode translations (OP_DISCARD for OpKill, etc.)
  }
}

// ============= Control Flow =============


} // namespace BWSL
