// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.

inline u16 IRLowering::LowerExpression(NodeRef ref) {
  if (ref.IsNull())
    return 0;

  // Recursion guard. Nested function-call / binary-op / ternary AST can
  // cycle or reach extreme depth from pathological inputs, blowing the
  // thread stack inside LowerFunctionCall -> LowerExpression -> ...
  // MAX_LOWER_DEPTH well exceeds any reasonable shader nesting but is
  // far below the ~1MB thread stack (each frame ~200 bytes).
  static constexpr u32 MAX_LOWER_DEPTH = 1024;
  if (lowerDepth >= MAX_LOWER_DEPTH) {
    return 0;
  }
  LowerDepthGuard _guard(lowerDepth);

  // Check if we already computed this (keyed by packed NodeRef)
  auto it = nodeRegisters.find(ref.packed);
  if (it != nodeRegisters.end()) {
    if (it->second != 0xFFFF) {
      return it->second;
    }
    nodeRegisters.erase(it);
  }

  u16 result = 0;

  switch (ref.Type()) {
  case ASTNodeType::LITERAL:
    result = LowerLiteral(ref);
    break;
  case ASTNodeType::IDENTIFIER:
    result = LowerIdentifier(ref);
    break;
  case ASTNodeType::BINARY_OP:
    result = LowerBinaryOp(ref);
    break;
  case ASTNodeType::UNARY_OP:
    result = LowerUnaryOp(ref);
    break;
  case ASTNodeType::TERNARY_EXPRESSION: {
    auto &tern = ast->GetTernaryExpression(ref);
    u16 condReg = LowerExpression(tern.condition);
    u16 trueReg = LowerExpression(tern.trueExpr);
    u16 falseReg = LowerExpression(tern.falseExpr);
    CoreType resultType = GetRegisterType(trueReg);

    // Matrix-typed ternary: pre-SPIR-V 1.4 `OpSelect` requires scalar or
    // vector types, so decompose a `cond ? matA : matB` into per-column
    // OpSelects and rebuild via OP_MAT_CONSTRUCT. Matches the struct
    // ternary decomposition below.
    if ((mask(resultType) & TypeMasks::MATRIX_TYPES) != 0) {
      u32 cols = (resultType == CoreType::MAT4)   ? 4
               : (resultType == CoreType::MAT3)   ? 3
                                                  : 2;
      CoreType colType = (resultType == CoreType::MAT4) ? CoreType::FLOAT4
                       : (resultType == CoreType::MAT3) ? CoreType::FLOAT3
                                                        : CoreType::FLOAT2;
      u16 colDests[4] = {0, 0, 0, 0};
      for (u32 c = 0; c < cols; c++) {
        u16 cIdx = EmitConstantInt(static_cast<int>(c));
        u16 colT = AllocateRegister();
        u16 colF = AllocateRegister();
        SetRegisterType(colT, colType);
        SetRegisterType(colF, colType);
        builder.EmitInstruction(OP_ARRAY_LOAD, colT, trueReg, cIdx);
        builder.EmitInstruction(OP_ARRAY_LOAD, colF, falseReg, cIdx);

        u16 pick = AllocateRegister();
        SetRegisterType(pick, colType);
        // OP_SELECT operand order is (false, true, cond)
        builder.EmitInstruction(OP_SELECT, pick, colF, colT, condReg);
        colDests[c] = pick;
      }
      u16 dest = AllocateRegister();
      builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                              colDests[1], colDests[2], colDests[3]);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(dest, resultType);
      return dest;
    }

    // Struct-typed ternary can't use a single OpSelect pre-SPIR-V 1.4.
    // Decompose into per-field OpSelect + composite reconstruction.
    if (resultType == CoreType::CUSTOM && trueReg < MAX_REGISTERS &&
        falseReg < MAX_REGISTERS) {
      u32 structHash = program.registerStructTypes[trueReg];
      if (structHash == 0) structHash = program.registerStructTypes[falseReg];
      if (structHash != 0) {
        auto it = structTypeMap.find(structHash);
        if (it != structTypeMap.end()) {
          const IRProgram::StructTypeInfo &info = program.structTypes[it->second];
          // Start from one of the operands and insert selected fields in place.
          u16 current = trueReg;
          for (u32 i = 0; i < info.fieldCount; i++) {
            u16 tf = AllocateRegister();
            builder.EmitInstruction(OP_STRUCT_EXTRACT, tf, trueReg, i);
            program.metadata[builder.currentInstruction - 1] = structHash;
            CoreType fieldType = static_cast<CoreType>(
                program.structFieldTypes[info.fieldOffset + i]);
            SetRegisterType(tf, fieldType);

            u16 ff = AllocateRegister();
            builder.EmitInstruction(OP_STRUCT_EXTRACT, ff, falseReg, i);
            program.metadata[builder.currentInstruction - 1] = structHash;
            SetRegisterType(ff, fieldType);

            u16 pick = AllocateRegister();
            builder.EmitInstruction(OP_SELECT, pick, ff, tf, condReg);
            SetRegisterType(pick, fieldType);

            u16 next = AllocateRegister();
            builder.EmitInstruction(OP_STRUCT_INSERT, next, current, i, pick);
            program.metadata[builder.currentInstruction - 1] = structHash;
            SetRegisterType(next, CoreType::CUSTOM);
            program.registerStructTypes[next] = structHash;
            current = next;
          }
          return current;
        }
      }
    }

    // Diagnose pointer-typed ternaries. A naive OpSelect on pointers
    // produces `Type Id is 0` SPIR-V validation errors (Function-storage
    // pointer select requires VariablePointers and is backend-specific).
    // Print an actionable diagnostic; downstream SPIR-V validation will
    // then fail the compile with a non-zero exit so the error is not
    // silently swallowed.
    if (trueReg < MAX_REGISTERS && falseReg < MAX_REGISTERS) {
      u32 tInfo = program.registerStorageInfo[trueReg];
      u32 fInfo = program.registerStorageInfo[falseReg];
      if ((tInfo & IR::IRProgram::STORAGE_IS_PTR) ||
          (fInfo & IR::IRProgram::STORAGE_IS_PTR)) {
        ReportError(
            "Error: ternary expression with pointer operands is not "
            "supported; select the dereferenced value instead "
            "(e.g. `(c ? pa^ : pb^)`) or branch on the pointer with "
            "if/else.\n");
      }
    }

    u16 dest = AllocateRegister();
    // Note: OP_SELECT order is (false, true, cond)
    builder.EmitInstruction(OP_SELECT, dest, falseReg, trueReg, condReg);
    SetRegisterType(dest, resultType);
    return dest;
  }
  case ASTNodeType::FUNCTION_CALL:
    result = LowerFunctionCall(ref);
    break;
  case ASTNodeType::MEMBER_ACCESS:
    result = LowerMemberAccess(ref);
    break;
  case ASTNodeType::ARRAY_ACCESS:
    result = LowerArrayAccess(ref);
    break;
  case ASTNodeType::BLOCK: {
    // Block as expression - lower statements and capture return value as
    // result
    const BlockData &block = ast->GetBlock(ref);
    result = 0;
    for (u32 i = 0; i < block.statements.count; i++) {
      NodeRef stmt = block.statements[i];
      // Check for RETURN statement - capture its value instead of emitting
      // return
      if (stmt.Type() == ASTNodeType::RETURN) {
        const AssignmentData &ret = ast->GetAssignment(stmt);
        if (!ret.value.IsNull()) {
          result = LowerExpression(ret.value);
        }
        // Don't emit the actual return, just capture the value
        break; // Return terminates the block
      }
      // Last statement might be an expression whose value is the block result
      else if (i == block.statements.count - 1) {
        // Try to lower as expression first
        switch (stmt.Type()) {
        case ASTNodeType::LITERAL:
        case ASTNodeType::IDENTIFIER:
        case ASTNodeType::BINARY_OP:
        case ASTNodeType::UNARY_OP:
        case ASTNodeType::FUNCTION_CALL:
        case ASTNodeType::MEMBER_ACCESS:
        case ASTNodeType::ARRAY_ACCESS:
          result = LowerExpression(stmt);
          break;
        default:
          // Lower as statement, result is 0
          LowerStatement(stmt);
          break;
        }
      } else {
        // Lower as statement
        LowerStatement(stmt);
      }
    }
    break;
  }
  default:
    result = AllocateRegister();
    break;
  }

  if (result != 0xFFFF) {
    nodeRegisters[ref.packed] = result;
  }
  return result;
}

inline u16 IRLowering::LowerUnaryOp(NodeRef ref) {
  const UnaryOpData &unop = ast->GetUnaryOp(ref);

  // Special case: address-of a struct-field member access (`^v.pos`).
  // Must fire before the generic LowerExpression path; otherwise
  // LowerMemberAccess emits OP_STRUCT_EXTRACT and we'd be pointing at a
  // by-value copy instead of the field's memory slot.
  if (unop.op == UnaryOpType::ADDRESS_OF &&
      unop.operand.Type() == ASTNodeType::MEMBER_ACCESS) {
    u16 fieldPtr = TryLowerLocalFieldAddressOf(unop.operand);
    if (fieldPtr != 0) return fieldPtr;
  }

  u16 operand = LowerExpression(unop.operand);
  u16 dest = AllocateRegister();

  OpCode op = OP_NOP;
  switch (unop.op) {
  case UnaryOpType::NEGATE: {
    CoreType type = GetRegisterType(operand);
    TypeMask tmask = mask(type);
    // Matrix types are float-valued but not in FLOAT_TYPES, so test them
    // explicitly to avoid falling into the integer path (which emits
    // OpSNegate on a float matrix and fails SPIR-V validation).
    bool isFloatLike = (tmask & TypeMasks::FLOAT_TYPES) ||
                       (tmask & TypeMasks::MATRIX_TYPES);
    op = isFloatLike ? OP_FNEG : OP_INEG;
    builder.EmitInstruction(op, dest, operand);
    SetRegisterType(dest, type); // Result has same type as operand
    return dest;
  }
  case UnaryOpType::NOT: {
    op = OP_NOT;
    CoreType type = GetRegisterType(operand);
    // Preserve bvec width — `!bvec3` must emit `OpLogicalNot %bvec3`, not
    // `%bool`. Scalar bools stay scalar.
    CoreType resultType = (type == CoreType::BOOL2 ||
                           type == CoreType::BOOL3 ||
                           type == CoreType::BOOL4)
                              ? type
                              : CoreType::BOOL;
    builder.EmitInstruction(op, dest, operand);
    SetRegisterType(dest, resultType);
    return dest;
  }
  case UnaryOpType::BITWISE_NOT: {
    CoreType type = GetRegisterType(operand);
    op = OP_NOT;
    builder.EmitInstruction(op, dest, operand);
    SetRegisterType(dest, type); // Bitwise NOT preserves type
    return dest;
  }
  case UnaryOpType::PRE_INCREMENT: {
    // ++x: Add 1 to operand, store back, return new value
    u16 one = EmitConstantInt(1);
    CoreType type = GetRegisterType(operand);
    op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FADD : OP_IADD;
    builder.EmitInstruction(op, dest, operand, one);
    SetRegisterType(dest, type);
    // Store back to the operand location (assuming it's a variable)
    builder.EmitInstruction(OP_STORE_REG, operand, dest);
    return dest;
  }
  case UnaryOpType::PRE_DECREMENT: {
    // --x: Subtract 1 from operand, store back, return new value
    u16 one = EmitConstantInt(1);
    CoreType type = GetRegisterType(operand);
    op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FSUB : OP_ISUB;
    builder.EmitInstruction(op, dest, operand, one);
    SetRegisterType(dest, type);
    builder.EmitInstruction(OP_STORE_REG, operand, dest);
    return dest;
  }
  case UnaryOpType::POST_INCREMENT: {
    // x++: Save old value, add 1 to operand, store back, return old value
    CoreType type = GetRegisterType(operand);
    builder.EmitInstruction(OP_STORE_REG, dest, operand); // dest = old value
    SetRegisterType(dest, type);
    u16 one = EmitConstantInt(1);
    u16 newVal = AllocateRegister();
    op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FADD : OP_IADD;
    builder.EmitInstruction(op, newVal, operand, one);
    SetRegisterType(newVal, type);
    builder.EmitInstruction(OP_STORE_REG, operand, newVal);
    return dest; // Return old value
  }
  case UnaryOpType::POST_DECREMENT: {
    // x--: Save old value, subtract 1 from operand, store back, return old
    // value
    CoreType type = GetRegisterType(operand);
    builder.EmitInstruction(OP_STORE_REG, dest, operand); // dest = old value
    SetRegisterType(dest, type);
    u16 one = EmitConstantInt(1);
    u16 newVal = AllocateRegister();
    op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FSUB : OP_ISUB;
    builder.EmitInstruction(op, newVal, operand, one);
    SetRegisterType(newVal, type);
    builder.EmitInstruction(OP_STORE_REG, operand, newVal);
    return dest; // Return old value
  }
  case UnaryOpType::ADDRESS_OF: {
    // ^x: Get pointer to variable. Rejects constants (bits 0x4000/0x8000),
    // out-of-range register indices, and registers past MAX_REGISTERS —
    // registerStorageInfo has only MAX_REGISTERS entries, and indexing with
    // a constant-encoded operand (>= 0x4000) reads/writes far OOB.
    if ((operand & 0xC000) != 0 || operand >= MAX_REGISTERS ||
        dest >= MAX_REGISTERS) {
      builder.EmitInstruction(OP_LOCAL_VAR_PTR, dest, operand);
      SetRegisterType(dest, CoreType::CUSTOM);
      return dest;
    }
    builder.EmitInstruction(OP_LOCAL_VAR_PTR, dest, operand);
    // Mark as pointer type - store pointee type and source register in
    // storage info Format: bits 0-7: flags, bits 8-15: pointee type, bits
    // 16-31: source register
    CoreType pointeeType = GetRegisterType(operand);
    SetRegisterType(dest, CoreType::CUSTOM); // Use CUSTOM to indicate pointer
    u32 storageVal = (static_cast<u32>(operand) << 16) |
                     (static_cast<u32>(pointeeType) << 8) |
                     IR::IRProgram::STORAGE_IS_PTR;
    builder.program->registerStorageInfo[dest] = storageVal;
    // Propagate the struct type hash so p^.field lowering can find the
    // struct layout. Without this, dereference falls back to vec-extract.
    if (program.registerStructTypes[operand] != 0) {
      program.registerStructTypes[dest] = program.registerStructTypes[operand];
    }
    // Mark the source register as address-taken so SSA doesn't create phi
    // nodes for it
    builder.program->registerStorageInfo[operand] |=
        IR::IRProgram::STORAGE_IS_ADDRESS_TAKEN;
    return dest;
  }
  case UnaryOpType::DEREFERENCE: {
    // x^: Dereference pointer.
    if ((operand & 0xC000) != 0 || operand >= MAX_REGISTERS) {
      builder.EmitInstruction(OP_LOCAL_LOAD, dest, operand);
      return dest;
    }
    u32 storageInfo = builder.program->registerStorageInfo[operand];
    if ((storageInfo & IR::IRProgram::STORAGE_IS_PTR) == 0) {
      // Operand isn't a pointer. This is a common parser-ambiguity
      // symptom: `a ^ -1` parses as `(a^) - 1` where `a` is a scalar
      // int, and the silent codegen used to emit OpLoad on a non-
      // pointer (rejected by SPIR-V validation, confusing error).
      // Explicit compile error: user should parenthesize the intended
      // operator — `a ^ (-1)` for XOR, `(ptr^) - n` for deref.
      ReportError("Error: dereference (`^` postfix) applied to a "
                  "non-pointer value. If you meant binary XOR with a "
                  "negative / unary-prefixed operand (e.g. `a ^ -1`), "
                  "wrap the right side in parentheses: `a ^ (-1)`.\n");
      SetRegisterType(dest, CoreType::INT);
      return dest;
    }
    CoreType pointeeType = static_cast<CoreType>((storageInfo >> 8) & 0xFF);
    builder.EmitInstruction(OP_LOCAL_LOAD, dest, operand);
    SetRegisterType(dest, pointeeType);
    // Propagate the struct type hash to the loaded value so member access
    // on `p^.field` can find the struct layout.
    if (program.registerStructTypes[operand] != 0) {
      program.registerStructTypes[dest] = program.registerStructTypes[operand];
    }
    return dest;
  }
  }

  // Fallthrough for unhandled cases (shouldn't happen)
  builder.EmitInstruction(op, dest, operand);
  return dest;
}

inline u16 IRLowering::LowerIdentifier(NodeRef ref) {
  const IdentifierData &ident = ast->GetIdentifier(ref);

  // Check if we already have a register for this variable
  auto it = variableRegisters.find(ident.name.nameHash);
  if (it != variableRegisters.end()) {
    return it->second;
  }

  // Look up in symbol table
  Symbol *sym = SymbolTable::LookupByHash(
      const_cast<SymbolTableData *>(symbols), ident.name.nameHash);
  if (sym && sym->kind == SymbolKind::VARIABLE) {
    const VariableData &varData = symbols->variables[sym->index];
    if (varData.isConst && varData.constExpr.IsValid()) {
      return LowerExpression(varData.constExpr);
    }
    // Allocate and cache
    u16 reg = AllocateRegister();
    variableRegisters[ident.name.nameHash] = reg;

    // Set register type from symbol's variable data
    SetRegisterType(reg, varData.typeInfo.coreType);

    return reg;
  }

  // Fallback: allocate register but emit a zero initialization
  // This handles undefined variables (likely a semantic error, but we need
  // valid IR)
  u16 reg = AllocateRegister();
  variableRegisters[ident.name.nameHash] = reg;
  // Emit a zero constant as fallback value to ensure the register is defined
  u16 zero = builder.EmitConstant(0.0f);
  builder.EmitInstruction(OP_STORE_REG, reg, zero);
  SetRegisterType(reg, CoreType::FLOAT); // Default to float
  return reg;
}

inline u16 IRLowering::LowerLiteral(NodeRef ref) {
  const LiteralData &lit = ast->GetLiteral(ref);
  switch (lit.value.type) {
  case LiteralValue::FLOAT:
    return builder.EmitConstant(lit.value.floatValue);
  case LiteralValue::INT:
    return EmitConstantInt(lit.value.intValue);
  case LiteralValue::UINT:
    return EmitConstantUint(lit.value.uintValue);
  case LiteralValue::BOOL:
    return builder.EmitConstantBool(lit.value.boolValue);
  default:
    return 0;
  }
}

inline u16 IRLowering::LowerBinaryOp(NodeRef ref) {
  const BinaryOpData &binop = ast->GetBinaryOp(ref);
  u16 left = LowerExpression(binop.left);
  u16 right = LowerExpression(binop.right);
  u16 dest = AllocateRegister();

  CoreType leftType = GetRegisterType(left);
  CoreType rightType = GetRegisterType(right);
  TypeMask leftMask = mask(leftType);
  TypeMask rightMask = mask(rightType);

  OpCode op = OP_NOP;
  CoreType resultType = leftType;

  // Handle matrix operations first (before float types, since mat4 isn't in
  // FLOAT_TYPES)
  if (leftMask & TypeMasks::MATRIX_TYPES) {
    if (binop.op == BinaryOpType::MULTIPLY) {
      if (rightMask & TypeMasks::MATRIX_TYPES) {
        // Matrix * Matrix
        op = OP_MAT_MUL;
        resultType = leftType; // mat4 * mat4 = mat4
      } else if (rightMask & TypeMasks::FLOAT_VECTORS) {
        // Matrix * Vector with dimension handling
        // For normal transformation: mat4 * float3 should return float3
        // (use upper-left 3x3 semantically, extend with w=0, multiply,
        // truncate)
        op = OP_MAT_VEC_MUL;

        u32 matDim = (leftType == CoreType::MAT4)   ? 4
                     : (leftType == CoreType::MAT3) ? 3
                                                    : 2;
        u32 vecDim = GetVectorDimension(rightType);

        if (vecDim < matDim) {
          // Vector is smaller than matrix - extend vector, multiply, truncate
          // This handles mat4 * float3 (normal transform) and mat4 * float2,
          // etc.

          // Step 1: Extend vector to match matrix dimension
          // For normals/directions, use w=0 (homogeneous direction)
          CoreType extendedType = (matDim == 4)   ? CoreType::FLOAT4
                                  : (matDim == 3) ? CoreType::FLOAT3
                                                  : CoreType::FLOAT2;
          u16 extendedVec = AllocateRegister();
          u16 zero = builder.EmitConstant(0.0f);

          // Construct extended vector: (vec.xyz, 0) for mat4*vec3, etc.
          if (vecDim == 3 && matDim == 4) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                    zero);
            program.metadata[builder.currentInstruction - 1] =
                4; // 4 components
          } else if (vecDim == 2 && matDim == 4) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                    zero, zero);
            program.metadata[builder.currentInstruction - 1] = 4;
          } else if (vecDim == 2 && matDim == 3) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                    zero);
            program.metadata[builder.currentInstruction - 1] = 3;
          } else {
            // Fallback: use original
            extendedVec = right;
            extendedType = rightType;
          }
          SetRegisterType(extendedVec, extendedType);

          // Step 2: Matrix * extended vector
          u16 fullResult = AllocateRegister();
          builder.EmitInstruction(OP_MAT_VEC_MUL, fullResult, left,
                                  extendedVec);
          SetRegisterType(fullResult, extendedType);

          // Step 3: Truncate result back to original vector dimension
          u32 shuffleMask = 0;
          for (u32 i = 0; i < vecDim; i++) {
            shuffleMask |= (i << (i * 8));
          }
          builder.EmitInstruction(OP_VEC_SHUFFLE, dest, fullResult,
                                  fullResult);
          program.metadata[builder.currentInstruction - 1] =
              shuffleMask | (vecDim << 24);
          SetRegisterType(dest,
                          rightType); // Result has same type as input vector
          return dest;
        } else {
          // Dimensions match or vector is larger (unusual but handle
          // gracefully) mat4 * vec4 = vec4, mat3 * vec3 = vec3, mat2 * vec2 =
          // vec2
          if (leftType == CoreType::MAT4)
            resultType = CoreType::FLOAT4;
          else if (leftType == CoreType::MAT3)
            resultType = CoreType::FLOAT3;
          else if (leftType == CoreType::MAT2)
            resultType = CoreType::FLOAT2;
        }
      } else if (rightMask & TypeMasks::SCALAR_TYPES) {
        // Matrix * Scalar
        op = OP_MAT_SCALE;
        resultType = leftType;
      }
    } else if ((binop.op == BinaryOpType::ADD ||
                binop.op == BinaryOpType::SUBTRACT ||
                binop.op == BinaryOpType::DIVIDE) &&
               (rightMask & TypeMasks::MATRIX_TYPES)) {
      // Element-wise matrix-matrix arithmetic. OpFAdd / OpFSub / OpFDiv
      // only accept scalar / vector types in SPIR-V, so decompose into
      // per-column column-extract + column-add + matrix-construct. This
      // matches what glslang / dxc emit for mat + mat in source code.
      u32 cols = (leftType == CoreType::MAT4)   ? 4
               : (leftType == CoreType::MAT3)   ? 3
                                                : 2;
      CoreType colType = (leftType == CoreType::MAT4)   ? CoreType::FLOAT4
                       : (leftType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                        : CoreType::FLOAT2;
      OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                   : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                          : OP_FDIV;

      u16 colDests[4] = {0, 0, 0, 0};
      for (u32 c = 0; c < cols; c++) {
        u16 idx = EmitConstantInt(static_cast<int>(c));
        u16 colL = AllocateRegister();
        u16 colR = AllocateRegister();
        SetRegisterType(colL, colType);
        SetRegisterType(colR, colType);
        builder.EmitInstruction(OP_ARRAY_LOAD, colL, left, idx);
        builder.EmitInstruction(OP_ARRAY_LOAD, colR, right, idx);

        u16 colDest = AllocateRegister();
        SetRegisterType(colDest, colType);
        builder.EmitInstruction(colOp, colDest, colL, colR);
        colDests[c] = colDest;
      }

      builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                              colDests[1], colDests[2], colDests[3]);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(dest, leftType);
      return dest;
    } else if ((binop.op == BinaryOpType::ADD ||
                binop.op == BinaryOpType::SUBTRACT ||
                binop.op == BinaryOpType::DIVIDE) &&
               (rightMask & TypeMasks::SCALAR_TYPES)) {
      // Matrix element-wise op scalar. SPIR-V has no OpMatrixPlusScalar —
      // splat the scalar into a column-sized vector and apply the op
      // per column, then reconstruct the matrix.
      u32 cols = (leftType == CoreType::MAT4)   ? 4
               : (leftType == CoreType::MAT3)   ? 3
                                                : 2;
      CoreType colType = (leftType == CoreType::MAT4)   ? CoreType::FLOAT4
                       : (leftType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                        : CoreType::FLOAT2;
      OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                   : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                          : OP_FDIV;

      u16 splat = AllocateRegister();
      builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, right, right, right,
                              right);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(splat, colType);

      u16 colDests[4] = {0, 0, 0, 0};
      for (u32 c = 0; c < cols; c++) {
        u16 idx = EmitConstantInt(static_cast<int>(c));
        u16 colL = AllocateRegister();
        SetRegisterType(colL, colType);
        builder.EmitInstruction(OP_ARRAY_LOAD, colL, left, idx);

        u16 colDest = AllocateRegister();
        SetRegisterType(colDest, colType);
        builder.EmitInstruction(colOp, colDest, colL, splat);
        colDests[c] = colDest;
      }

      builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                              colDests[1], colDests[2], colDests[3]);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(dest, leftType);
      return dest;
    }
    if (op != OP_NOP) {
      builder.EmitInstruction(op, dest, left, right);
      SetRegisterType(dest, resultType);
      return dest;
    }
  } else if ((leftMask & TypeMasks::SCALAR_TYPES) &&
             (rightMask & TypeMasks::MATRIX_TYPES)) {
    // Scalar * Matrix is commutative with the matrix-scalar path;
    // reorder so OP_MAT_SCALE sees (matrix, scalar). Without this the
    // expression falls into the scalar-arithmetic branch and emits
    // OpFMul with a scalar result on matrix operands.
    if (binop.op == BinaryOpType::MULTIPLY) {
      builder.EmitInstruction(OP_MAT_SCALE, dest, right, left);
      SetRegisterType(dest, rightType);
      return dest;
    }
    if (binop.op == BinaryOpType::ADD ||
        binop.op == BinaryOpType::SUBTRACT ||
        binop.op == BinaryOpType::DIVIDE) {
      // Scalar element-wise op matrix. Same splat-then-per-column
      // strategy as the matrix-on-left case. Operand order matters
      // for SUBTRACT / DIVIDE: emit FOP(splat, col).
      u32 cols = (rightType == CoreType::MAT4)   ? 4
               : (rightType == CoreType::MAT3)   ? 3
                                                : 2;
      CoreType colType = (rightType == CoreType::MAT4)   ? CoreType::FLOAT4
                       : (rightType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                        : CoreType::FLOAT2;
      OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                   : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                          : OP_FDIV;

      u16 splat = AllocateRegister();
      builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, left, left, left,
                              left);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(splat, colType);

      u16 colDests[4] = {0, 0, 0, 0};
      for (u32 c = 0; c < cols; c++) {
        u16 idx = EmitConstantInt(static_cast<int>(c));
        u16 colR = AllocateRegister();
        SetRegisterType(colR, colType);
        builder.EmitInstruction(OP_ARRAY_LOAD, colR, right, idx);

        u16 colDest = AllocateRegister();
        SetRegisterType(colDest, colType);
        builder.EmitInstruction(colOp, colDest, splat, colR);
        colDests[c] = colDest;
      }

      builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                              colDests[1], colDests[2], colDests[3]);
      program.metadata[builder.currentInstruction - 1] = cols;
      SetRegisterType(dest, rightType);
      return dest;
    }
  } else if ((leftMask & TypeMasks::FLOAT_VECTORS) &&
             (rightMask & TypeMasks::MATRIX_TYPES)) {
    // Vector * Matrix with dimension handling
    if (binop.op == BinaryOpType::MULTIPLY) {
      op = OP_VEC_MAT_MUL;

      u32 vecDim = GetVectorDimension(leftType);
      u32 matDim = (rightType == CoreType::MAT4)   ? 4
                   : (rightType == CoreType::MAT3) ? 3
                                                   : 2;

      if (vecDim < matDim) {
        // Vector is smaller than matrix - extend, multiply, truncate
        CoreType extendedType = (matDim == 4)   ? CoreType::FLOAT4
                                : (matDim == 3) ? CoreType::FLOAT3
                                                : CoreType::FLOAT2;
        u16 extendedVec = AllocateRegister();
        u16 zero = builder.EmitConstant(0.0f);

        if (vecDim == 3 && matDim == 4) {
          builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero);
          program.metadata[builder.currentInstruction - 1] = 4;
        } else if (vecDim == 2 && matDim == 4) {
          builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero,
                                  zero);
          program.metadata[builder.currentInstruction - 1] = 4;
        } else if (vecDim == 2 && matDim == 3) {
          builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero);
          program.metadata[builder.currentInstruction - 1] = 3;
        } else {
          extendedVec = left;
          extendedType = leftType;
        }
        SetRegisterType(extendedVec, extendedType);

        // Vector * Matrix
        u16 fullResult = AllocateRegister();
        builder.EmitInstruction(OP_VEC_MAT_MUL, fullResult, extendedVec,
                                right);
        SetRegisterType(fullResult, extendedType);

        // Truncate back to original dimension
        u32 shuffleMask = 0;
        for (u32 i = 0; i < vecDim; i++) {
          shuffleMask |= (i << (i * 8));
        }
        builder.EmitInstruction(OP_VEC_SHUFFLE, dest, fullResult, fullResult);
        program.metadata[builder.currentInstruction - 1] =
            shuffleMask | (vecDim << 24);
        SetRegisterType(dest, leftType);
        return dest;
      } else {
        // Dimensions match or vector is larger
        resultType = leftType;
        builder.EmitInstruction(op, dest, left, right);
        SetRegisterType(dest, resultType);
        return dest;
      }
    }
  }

  // Use leftMask for type dispatch (renamed from typeMask for clarity)
  TypeMask typeMask = leftMask;

  // Handle vector dimension mismatch: if both operands are vectors but with
  // different dimensions, truncate the larger vector to match the smaller
  // one. This handles cases like: vec4 - vec3 -> truncate vec4 to vec3, then
  // subtract
  u32 leftDim = GetVectorDimension(leftType);
  u32 rightDim = GetVectorDimension(rightType);

  // Handle vector-scalar ops by splatting the scalar to a matching vector.
  if ((leftDim > 1 && rightDim == 1) || (rightDim > 1 && leftDim == 1)) {
    bool leftIsVector = (leftDim > 1);
    CoreType vectorType = leftIsVector ? leftType : rightType;
    CoreType scalarType = leftIsVector ? rightType : leftType;
    CoreType vectorScalarType = CoreType::INVALID;
    switch (vectorType) {
    case CoreType::FLOAT2:
    case CoreType::FLOAT3:
    case CoreType::FLOAT4:
      vectorScalarType = CoreType::FLOAT;
      break;
    case CoreType::INT2:
    case CoreType::INT3:
    case CoreType::INT4:
      vectorScalarType = CoreType::INT;
      break;
    case CoreType::UINT2:
    case CoreType::UINT3:
    case CoreType::UINT4:
      vectorScalarType = CoreType::UINT;
      break;
    default:
      break;
    }

    if (vectorScalarType != CoreType::INVALID &&
        scalarType == vectorScalarType) {
      u16 scalarReg = leftIsVector ? right : left;
      u16 splat = AllocateRegister();
      builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, scalarReg, scalarReg,
                              scalarReg, scalarReg);
      program.metadata[builder.currentInstruction - 1] =
          GetVectorDimension(vectorType);
      SetRegisterType(splat, vectorType);

      if (leftIsVector) {
        right = splat;
        rightType = vectorType;
        rightMask = mask(rightType);
      } else {
        left = splat;
        leftType = vectorType;
        leftMask = mask(leftType);
      }

      leftDim = GetVectorDimension(leftType);
      rightDim = GetVectorDimension(rightType);
      resultType = leftType;
    }
  }

  if (leftDim > 1 && rightDim > 1 && leftDim != rightDim) {
    u32 targetDim = (leftDim < rightDim) ? leftDim : rightDim;
    CoreType targetType = GetVectorTypeWithDimension(leftType, targetDim);

    if (leftDim > targetDim) {
      // Truncate left operand using VEC_SHUFFLE
      u16 truncated = AllocateRegister();
      // Emit VEC_SHUFFLE with identity indices for first targetDim components
      // The metadata encodes the shuffle mask: component indices 0, 1, 2 for
      // vec3
      u32 shuffleMask = 0;
      for (u32 i = 0; i < targetDim; i++) {
        shuffleMask |= (i << (i * 8)); // Each component index in 8 bits
      }
      builder.EmitInstruction(OP_VEC_SHUFFLE, truncated, left, left);
      program.metadata[builder.currentInstruction - 1] =
          shuffleMask | (targetDim << 24);
      SetRegisterType(truncated, targetType);
      left = truncated;
      leftType = targetType;
      leftMask = mask(leftType);
      typeMask = leftMask;
    }

    if (rightDim > targetDim) {
      // Truncate right operand using VEC_SHUFFLE
      u16 truncated = AllocateRegister();
      u32 shuffleMask = 0;
      for (u32 i = 0; i < targetDim; i++) {
        shuffleMask |= (i << (i * 8));
      }
      builder.EmitInstruction(OP_VEC_SHUFFLE, truncated, right, right);
      program.metadata[builder.currentInstruction - 1] =
          shuffleMask | (targetDim << 24);
      SetRegisterType(truncated, targetType);
      right = truncated;
      rightType = targetType;
      rightMask = mask(rightType);
    }

    resultType = targetType;
  }

  if (typeMask & TypeMasks::FLOAT_TYPES) {
    switch (binop.op) {
    case BinaryOpType::ADD:
      op = OP_FADD;
      break;
    case BinaryOpType::SUBTRACT:
      op = OP_FSUB;
      break;
    case BinaryOpType::MULTIPLY:
      op = OP_FMUL;
      break;
    case BinaryOpType::DIVIDE:
      op = OP_FDIV;
      break;
    case BinaryOpType::MODULO:
      op = OP_FMOD;
      break;
    case BinaryOpType::EQUALS:
      op = OP_FEQ;
      break;
    case BinaryOpType::NOT_EQUALS:
      op = OP_FNE;
      break;
    case BinaryOpType::LESS:
      op = OP_FLT;
      break;
    case BinaryOpType::LESS_EQUAL:
      op = OP_FLE;
      break;
    case BinaryOpType::GREATER:
      op = OP_FGT;
      break;
    case BinaryOpType::GREATER_EQUAL:
      op = OP_FGE;
      break;
    case BinaryOpType::AND:
      op = OP_AND;
      break;
    case BinaryOpType::OR:
      op = OP_OR;
      break;
    default:
      op = OP_NOP;
      break;
    }
  } else if (typeMask & TypeMasks::UINT_TYPES) {
    switch (binop.op) {
    case BinaryOpType::ADD:
      op = OP_IADD;
      break;
    case BinaryOpType::SUBTRACT:
      op = OP_ISUB;
      break;
    case BinaryOpType::MULTIPLY:
      op = OP_IMUL;
      break;
    case BinaryOpType::DIVIDE:
      op = OP_IDIV;
      break; // Note: should use UDIV
    case BinaryOpType::MODULO:
      op = OP_IMOD;
      break;
    case BinaryOpType::EQUALS:
      op = OP_IEQ;
      break;
    case BinaryOpType::NOT_EQUALS:
      op = OP_INE;
      break;
    case BinaryOpType::LESS:
      op = OP_ULT;
      break;
    case BinaryOpType::LESS_EQUAL:
      op = OP_ULE;
      break;
    case BinaryOpType::GREATER:
      op = OP_UGT;
      break;
    case BinaryOpType::GREATER_EQUAL:
      op = OP_UGE;
      break;
    case BinaryOpType::AND:
      op = OP_AND;
      break;
    case BinaryOpType::OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_AND:
      op = OP_AND;
      break;
    case BinaryOpType::BITWISE_OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_XOR:
      op = OP_XOR;
      break;
    case BinaryOpType::LEFT_SHIFT:
      op = OP_SHL;
      break;
    case BinaryOpType::RIGHT_SHIFT:
      op = OP_SHR;
      break;
    default:
      op = OP_NOP;
      break;
    }
  } else if (typeMask & TypeMasks::INT_TYPES) {
    switch (binop.op) {
    case BinaryOpType::ADD:
      op = OP_IADD;
      break;
    case BinaryOpType::SUBTRACT:
      op = OP_ISUB;
      break;
    case BinaryOpType::MULTIPLY:
      op = OP_IMUL;
      break;
    case BinaryOpType::DIVIDE:
      op = OP_IDIV;
      break;
    case BinaryOpType::MODULO:
      op = OP_IMOD;
      break;
    case BinaryOpType::EQUALS:
      op = OP_IEQ;
      break;
    case BinaryOpType::NOT_EQUALS:
      op = OP_INE;
      break;
    case BinaryOpType::LESS:
      op = OP_ILT;
      break;
    case BinaryOpType::LESS_EQUAL:
      op = OP_ILE;
      break;
    case BinaryOpType::GREATER:
      op = OP_IGT;
      break;
    case BinaryOpType::GREATER_EQUAL:
      op = OP_IGE;
      break;
    case BinaryOpType::AND:
      op = OP_AND;
      break;
    case BinaryOpType::OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_AND:
      op = OP_AND;
      break;
    case BinaryOpType::BITWISE_OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_XOR:
      op = OP_XOR;
      break;
    case BinaryOpType::LEFT_SHIFT:
      op = OP_SHL;
      break;
    case BinaryOpType::RIGHT_SHIFT:
      op = OP_ASR;
      break; // Arithmetic shift for signed
    default:
      op = OP_NOP;
      break;
    }
  } else {
    // Bool or other types
    switch (binop.op) {
    case BinaryOpType::EQUALS:
      op = OP_IEQ;
      break;
    case BinaryOpType::NOT_EQUALS:
      op = OP_INE;
      break;
    case BinaryOpType::AND:
      op = OP_AND;
      break;
    case BinaryOpType::OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_AND:
      op = OP_AND;
      break;
    case BinaryOpType::BITWISE_OR:
      op = OP_OR;
      break;
    case BinaryOpType::BITWISE_XOR:
      op = OP_XOR;
      break;
    case BinaryOpType::LEFT_SHIFT:
      op = OP_SHL;
      break;
    case BinaryOpType::RIGHT_SHIFT:
      op = OP_SHR;
      break;
    default:
      op = OP_NOP;
      break;
    }
  }

  builder.EmitInstruction(op, dest, left, right);

  // Comparison operations produce BOOL, other operations produce the operand
  // type
  bool isComparison = (binop.op == BinaryOpType::EQUALS ||
                       binop.op == BinaryOpType::NOT_EQUALS ||
                       binop.op == BinaryOpType::LESS ||
                       binop.op == BinaryOpType::LESS_EQUAL ||
                       binop.op == BinaryOpType::GREATER ||
                       binop.op == BinaryOpType::GREATER_EQUAL);

  // For scalar-vector operations, the result type is the wider type (vector)
  // This handles cases like: 1.0 - vec2, 1.0 * vec3, etc.
  if (!isComparison) {
    bool leftIsScalar = (leftMask & TypeMasks::SCALAR_TYPES) != 0;
    bool rightIsVector =
        (rightMask & (TypeMasks::FLOAT_VECTORS | TypeMasks::INT_VECTORS |
                      TypeMasks::UINT_VECTORS)) != 0;
    if (leftIsScalar && rightIsVector) {
      resultType = rightType; // Use the vector type
    }
  }

  SetRegisterType(dest, isComparison ? CoreType::BOOL : resultType);
  return dest;
}

