// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.
#pragma once
#include "bwsl_ir_lowering.h"

namespace BWSL::IR {

namespace {

inline bool DecodeSwizzleByHash(u32 nameHash, u16 lengthHint, u8 outIndices[4],
                                u32 *outLength) {
  const char xyzw[] = {'x', 'y', 'z', 'w'};
  const char rgba[] = {'r', 'g', 'b', 'a'};
  const char *sets[] = {xyzw, rgba};

  u32 minLen = 2;
  u32 maxLen = 4;
  if (lengthHint >= 2 && lengthHint <= 4) {
    minLen = lengthHint;
    maxLen = lengthHint;
  }

  for (u32 setIdx = 0; setIdx < 2; setIdx++) {
    const char *chars = sets[setIdx];
    for (u32 len = minLen; len <= maxLen; len++) {
      u32 combinations = 1u << (len * 2); // 4^len
      for (u32 n = 0; n < combinations; n++) {
        char candidate[4] = {};
        u8 indices[4] = {};
        for (u32 i = 0; i < len; i++) {
          u32 shift = (len - 1 - i) * 2;
          u8 idx = static_cast<u8>((n >> shift) & 0x3);
          candidate[i] = chars[idx];
          indices[i] = idx;
        }
        if (Utils::HashStr(candidate, static_cast<u16>(len)) == nameHash) {
          for (u32 i = 0; i < len; i++) {
            outIndices[i] = indices[i];
          }
          *outLength = len;
          return true;
        }
      }
    }
  }

  return false;
}

inline u32 FindStructTypeIndex(IRLowering *lowering, u32 structTypeHash) {
  if (!lowering || structTypeHash == 0) return 0xFFFFFFFF;

  u32 canonicalHash = lowering->LookupOrRegisterStructType(structTypeHash);
  if (canonicalHash != 0) {
    auto canonicalIt = lowering->structTypeMap.find(canonicalHash);
    if (canonicalIt != lowering->structTypeMap.end()) {
      return canonicalIt->second;
    }
  }

  auto directIt = lowering->structTypeMap.find(structTypeHash);
  if (directIt != lowering->structTypeMap.end()) {
    return directIt->second;
  }

  return 0xFFFFFFFF;
}

inline u32 ResolveStructHashForStaticAccess(IRLowering *lowering, NodeRef expr) {
  if (!lowering || expr.IsNull()) return 0;

  switch (expr.Type()) {
  case ASTNodeType::IDENTIFIER: {
    const IdentifierData &ident = lowering->ast->GetIdentifier(expr);
    auto varIt = lowering->variableStructTypes.find(ident.name.nameHash);
    if (varIt != lowering->variableStructTypes.end()) {
      return varIt->second;
    }

    Symbol *sym = SymbolTable::LookupAny(
        const_cast<SymbolTableData *>(lowering->symbols), ident.name);
    if (sym && sym->kind == SymbolKind::VARIABLE &&
        sym->index < lowering->symbols->variables.count) {
      const VariableData &varData = lowering->symbols->variables[sym->index];
      if (varData.typeInfo.coreType == CoreType::CUSTOM ||
          varData.typeInfo.coreType == CoreType::ENUM) {
        return varData.typeInfo.customTypeHash;
      }
    }
    return 0;
  }

  case ASTNodeType::MEMBER_ACCESS: {
    const MemberAccessData &access = lowering->ast->GetMemberAccess(expr);

    if (access.object.Type() == ASTNodeType::IDENTIFIER) {
      const IdentifierData &obj = lowering->ast->GetIdentifier(access.object);
      if (obj.identifierKind == SpecialIdentifier::RESOURCES) {
        Symbol *sym = SymbolTable::LookupResource(
            const_cast<SymbolTableData *>(lowering->symbols), access.member);
        if (sym && sym->index < lowering->symbols->resources.count) {
          const ResourceData &resData = lowering->symbols->resources[sym->index];
          if (resData.structTypeHash != 0) {
            return resData.structTypeHash;
          }
          if (static_cast<CoreType>(resData.coreType) == CoreType::CUSTOM) {
            return resData.typeName.nameHash;
          }
        }
        return 0;
      }
    }

    u32 objectStructHash =
        ResolveStructHashForStaticAccess(lowering, access.object);
    u32 structIdx = FindStructTypeIndex(lowering, objectStructHash);
    if (structIdx == 0xFFFFFFFF) return 0;

    const IRProgram::StructTypeInfo &info =
        lowering->program.structTypes[structIdx];
    for (u32 i = 0; i < info.fieldCount; i++) {
      u32 fieldOffset = info.fieldOffset + i;
      if (lowering->program.structFieldNameHashes[fieldOffset] !=
          access.member.nameHash) {
        continue;
      }

      CoreType fieldType =
          static_cast<CoreType>(lowering->program.structFieldTypes[fieldOffset]);
      if (fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) {
        return lowering->program.structFieldTypeHashes[fieldOffset];
      }
      return 0;
    }
    return 0;
  }

  case ASTNodeType::ARRAY_ACCESS: {
    const ArrayAccessData &arrayAccess = lowering->ast->GetArrayAccess(expr);
    return ResolveStructHashForStaticAccess(lowering, arrayAccess.array);
  }

  default:
    return 0;
  }
}

inline u32 ResolveStaticArrayLength(IRLowering *lowering, NodeRef expr) {
  if (!lowering || expr.IsNull()) return 0;

  if (expr.Type() == ASTNodeType::IDENTIFIER) {
    const IdentifierData &ident = lowering->ast->GetIdentifier(expr);
    Symbol *sym = SymbolTable::LookupAny(
        const_cast<SymbolTableData *>(lowering->symbols), ident.name);
    if (sym && sym->kind == SymbolKind::VARIABLE &&
        sym->index < lowering->symbols->variables.count) {
      return lowering->symbols->variables[sym->index].typeInfo.arrayLength;
    }
    return 0;
  }

  if (expr.Type() != ASTNodeType::MEMBER_ACCESS) {
    return 0;
  }

  const MemberAccessData &access = lowering->ast->GetMemberAccess(expr);
  u32 objectStructHash =
      ResolveStructHashForStaticAccess(lowering, access.object);
  u32 structIdx = FindStructTypeIndex(lowering, objectStructHash);
  if (structIdx == 0xFFFFFFFF) return 0;

  const IRProgram::StructTypeInfo &info =
      lowering->program.structTypes[structIdx];
  for (u32 i = 0; i < info.fieldCount; i++) {
    u32 fieldOffset = info.fieldOffset + i;
    if (lowering->program.structFieldNameHashes[fieldOffset] ==
        access.member.nameHash) {
      return lowering->program.structFieldArraySizes[fieldOffset];
    }
  }

  return 0;
}

} // namespace

inline u16 IRLowering::TryLowerLocalFieldAddressOf(NodeRef memberRef) {
  const MemberAccessData &access = ast->GetMemberAccess(memberRef);

  if (access.object.Type() != ASTNodeType::IDENTIFIER) return 0;
  const IdentifierData &obj = ast->GetIdentifier(access.object);
  if (obj.identifierKind != SpecialIdentifier::NONE) return 0;

  // The identifier must be a user variable we've already allocated a
  // register for. LowerIdentifier handles symbol lookup + caching.
  u16 baseReg = LowerIdentifier(access.object);
  if (baseReg >= MAX_REGISTERS) return 0;

  // Base must carry a struct type hash (either from declaration or
  // inferred). Swizzle / vector-component cases hit when the base
  // type is a vector with no struct hash — bail and let the existing
  // ADDRESS_OF path reject them.
  u32 structTypeHash = program.registerStructTypes[baseReg];
  if (structTypeHash == 0) {
    auto varIt = variableStructTypes.find(obj.name.nameHash);
    if (varIt != variableStructTypes.end()) {
      structTypeHash = varIt->second;
      program.registerStructTypes[baseReg] = structTypeHash;
    }
  }
  if (structTypeHash == 0) return 0;

  auto structIt = structTypeMap.find(structTypeHash);
  if (structIt == structTypeMap.end()) return 0;

  const IRProgram::StructTypeInfo &info = program.structTypes[structIt->second];
  u32 fieldIndex = 0xFFFFFFFF;
  CoreType fieldType = CoreType::FLOAT;
  u32 fieldTypeHash = 0;
  for (u32 i = 0; i < info.fieldCount; i++) {
    if (program.structFieldNameHashes[info.fieldOffset + i] ==
        access.member.nameHash) {
      fieldIndex = i;
      fieldType = static_cast<CoreType>(
          program.structFieldTypes[info.fieldOffset + i]);
      if (program.structFieldTypeHashes) {
        fieldTypeHash = program.structFieldTypeHashes[info.fieldOffset + i];
      }
      break;
    }
  }
  if (fieldIndex == 0xFFFFFFFF) return 0;

  u16 dest = AllocateRegister();
  if (dest >= MAX_REGISTERS) return 0;

  // Emit: dest = &base.field
  // operand1 carries the literal field index (backends read it via
  // GetOperand(i, 1); SSA skips operand 1 of field-ptr renaming).
  builder.EmitInstruction(OP_LOCAL_FIELD_PTR, dest, baseReg,
                          static_cast<u16>(fieldIndex));
  program.metadata[builder.currentInstruction - 1] = structTypeHash;

  // Pointer value has "no legitimate CoreType" — mark as CUSTOM, same
  // as scalar pointers. Pointee type + base reg + field flags live
  // in registerStorageInfo so OP_LOCAL_LOAD / OP_LOCAL_STORE can
  // recover them.
  SetRegisterType(dest, CoreType::CUSTOM);
  u32 storageVal = (static_cast<u32>(baseReg) << 16) |
                   (static_cast<u32>(fieldType) << 8) |
                   IR::IRProgram::STORAGE_IS_PTR |
                   IR::IRProgram::STORAGE_IS_FIELD_PTR;
  program.registerStorageInfo[dest] = storageVal;

  // Base struct variable's address is effectively taken — SSA must
  // leave its register alone.
  program.registerStorageInfo[baseReg] |=
      IR::IRProgram::STORAGE_IS_ADDRESS_TAKEN;

  // If the field is itself a struct/enum, propagate its type hash so
  // chained member access on the dereferenced pointer sees the right
  // struct info.
  if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
      fieldTypeHash != 0) {
    program.registerStructTypes[dest] = fieldTypeHash;
  }

  return dest;
}

inline void IRLowering::LowerVariableDecl(NodeRef ref) {
  const VariableDeclData &varDecl = ast->GetVariableDecl(ref);

  Symbol *sym = SymbolTable::LookupByHash(
      const_cast<SymbolTableData *>(symbols), varDecl.name.nameHash);
  const VariableData *varData = (sym && sym->kind == SymbolKind::VARIABLE)
                                    ? &symbols->variables[sym->index]
                                    : nullptr;
  bool isShared = (varDecl.storageClass == StorageClass::Shared) ||
                  (varData && varData->storageClass == StorageClass::Shared);
  bool isArray = varData && IsArray(varData->typeInfo);
  u32 arrayLength = isArray ? varData->typeInfo.arrayLength : 0;
  if (!isArray && varDecl.arrayDimensions > 0) {
    isArray = true;
    arrayLength = varDecl.arrayLength;
  }

  // Allocate register for the variable
  u16 varReg = AllocateRegister();

  // Store mapping from variable name hash to register
  variableRegisters[varDecl.name.nameHash] = varReg;
  if (varDecl.isConst) {
    constVariables.insert(varDecl.name.nameHash);
  }

  // Track type from the declaration first to avoid symbol-table collisions
  // for locals.
  u32 customTypeHash = 0;
  CoreType coreType =
      ResolveCoreTypeFromHash(varDecl.type.nameHash, &customTypeHash);
  if ((coreType == CoreType::INVALID || coreType == CoreType::VOID) &&
      varData && varData->typeInfo.coreType != CoreType::INVALID) {
    coreType = varData->typeInfo.coreType;
    customTypeHash = varData->typeInfo.customTypeHash;
  }
  if ((coreType == CoreType::INVALID || coreType == CoreType::VOID) &&
      varDecl.arrayElementTypeHash != 0) {
    coreType = ResolveCoreTypeFromHash(varDecl.arrayElementTypeHash,
                                       &customTypeHash);
  }

  if (isShared) {
    if (currentStage != ShaderStage::Compute) {
      ReportError(
          "Error: shared variables are only allowed in compute shaders\n");
    }
    if (!isArray) {
      ReportError("Error: shared variables must be declared as arrays\n");
    }

    SetRegisterType(varReg, coreType);

    if (program.sharedVarCount >= program.sharedVarCapacity) {
      u32 newCapacity = program.sharedVarCapacity * 2;
      u32 *newNameHashes =
          (u32 *)pool->Allocate(newCapacity * sizeof(u32), 64);
      u16 *newTypes = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
      u32 *newArraySizes =
          (u32 *)pool->Allocate(newCapacity * sizeof(u32), 64);
      u16 *newRegisters =
          (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
      memcpy(newNameHashes, program.sharedNameHashes,
             program.sharedVarCapacity * sizeof(u32));
      memcpy(newTypes, program.sharedTypes,
             program.sharedVarCapacity * sizeof(u16));
      memcpy(newArraySizes, program.sharedArraySizes,
             program.sharedVarCapacity * sizeof(u32));
      memcpy(newRegisters, program.sharedRegisters,
             program.sharedVarCapacity * sizeof(u16));
      program.sharedNameHashes = newNameHashes;
      program.sharedTypes = newTypes;
      program.sharedArraySizes = newArraySizes;
      program.sharedRegisters = newRegisters;
      program.sharedVarCapacity = newCapacity;
    }

    u32 sharedIndex = program.sharedVarCount++;
    program.sharedNameHashes[sharedIndex] = varDecl.name.nameHash;
    program.sharedTypes[sharedIndex] = static_cast<u16>(coreType);
    program.sharedArraySizes[sharedIndex] = isArray ? arrayLength : 0;
    program.sharedRegisters[sharedIndex] = varReg;

    program.registerStorageInfo[varReg] =
        (sharedIndex << IR::IRProgram::STORAGE_BINDING_SHIFT) |
        IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_SHARED;
    return;
  }

  // Check if this is a struct or enum sum type
  if (coreType == CoreType::CUSTOM || coreType == CoreType::INVALID) {
    u32 lookupHash =
        (customTypeHash != 0) ? customTypeHash : varDecl.type.nameHash;
    u32 structTypeHash = LookupOrRegisterStructType(lookupHash);
    if (structTypeHash != 0) {
      coreType = CoreType::CUSTOM;
      variableStructTypes[varDecl.name.nameHash] = structTypeHash;
      program.registerStructTypes[varReg] = structTypeHash;
    }
  }

  SetRegisterType(varReg, coreType);

  CoreType arrayElementType = coreType;
  u32 arrayElementStructHash = 0;

  // Register local arrays (with or without initializer)
  if (isArray && arrayLength > 0) {
    // Get element type from the variable's array element type hash
    if (varDecl.arrayElementTypeHash != 0) {
      arrayElementType = ResolveCoreTypeFromHash(varDecl.arrayElementTypeHash,
                                                 &arrayElementStructHash);
    } else if (varData) {
      arrayElementType = varData->typeInfo.coreType;
      arrayElementStructHash = varData->typeInfo.customTypeHash;
    }
    // For struct element types, ensure we have the struct type registered
    if ((arrayElementType == CoreType::CUSTOM ||
         arrayElementType == CoreType::INVALID) &&
        arrayElementStructHash == 0 && varDecl.arrayElementTypeHash != 0) {
      arrayElementStructHash =
          LookupOrRegisterStructType(varDecl.arrayElementTypeHash);
      if (arrayElementStructHash != 0) {
        arrayElementType = CoreType::CUSTOM;
      }
    }

    // Register the array for local array tracking
    program.localArrayNameHashes[program.localArrayCount] =
        varDecl.name.nameHash;
    program.localArrayTypes[program.localArrayCount] =
        static_cast<u16>(arrayElementType);
    program.localArrayStructTypes[program.localArrayCount] =
        arrayElementStructHash;
    program.localArraySizes[program.localArrayCount] = arrayLength;
    program.localArrayRegisters[program.localArrayCount] = varReg;
    program.localArrayCount++;

    // Mark the register as an array pointer
    program.registerStorageInfo[varReg] =
        ((program.localArrayCount - 1)
         << IR::IRProgram::STORAGE_BINDING_SHIFT) |
        IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_LOCAL_ARRAY;
  }

  // If there's an initializer, evaluate it
  if (!varDecl.initializer.IsNull()) {
    // Check for array initializer (BLOCK node with element expressions)
    if (isArray && varDecl.initializer.Type() == ASTNodeType::BLOCK) {
      // Array with block initializer - emit element-by-element stores
      const BlockData &initBlock = ast->GetBlock(varDecl.initializer);
      for (u32 i = 0; i < initBlock.statements.count && i < arrayLength;
           i++) {
        NodeRef elementExpr = initBlock.statements[i];
        u16 elementReg = LowerExpression(elementExpr);
        elementReg = ConvertRegisterToType(elementReg, arrayElementType);

        // Emit array store: store element at index i
        u16 indexReg = EmitConstantUint(i);
        builder.EmitInstruction(OP_ARRAY_STORE, varReg, indexReg, elementReg);
      }
      initializedVariables.insert(varDecl.name.nameHash);
    } else {
      u16 initReg = LowerExpression(varDecl.initializer);

      // If the variable has a known struct type and the initializer doesn't,
      // propagate the struct type to the initializer result. This handles
      // cases like loading from untyped storage buffers into typed variables.
      if (coreType == CoreType::CUSTOM && initReg < MAX_REGISTERS) {
        u32 varStructHash = variableStructTypes[varDecl.name.nameHash];
        if (varStructHash == 0) {
          varStructHash = program.registerStructTypes[varReg];
        }
        if (varStructHash == 0 && program.registerStructTypes[initReg] != 0) {
          varStructHash = program.registerStructTypes[initReg];
          variableStructTypes[varDecl.name.nameHash] = varStructHash;
          program.registerStructTypes[varReg] = varStructHash;
        }
        if (varStructHash != 0 && program.registerStructTypes[initReg] == 0) {
          program.registerStructTypes[initReg] = varStructHash;
          SetRegisterType(initReg, CoreType::CUSTOM);

          // If the initializer came from a storage buffer load, record the
          // element struct type for this binding
          u32 storageInfo = program.registerStorageInfo[initReg];
          if (storageInfo != 0) {
            u32 binding =
                (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
            if (binding < 32) {
              program.bufferElementStructTypes[binding] = varStructHash;
            }
          }
        }
      }

      // If the initializer came from a storage buffer load, record the
      // element type for this binding (needed for SPIR-V backend to declare
      // correct buffer element types)
      if (initReg < MAX_REGISTERS && coreType != CoreType::CUSTOM) {
        u32 storageInfo = program.registerStorageInfo[initReg];
        if (storageInfo != 0) {
          u32 binding = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
          if (binding < 32 && program.bufferElementCoreTypes[binding] == 0) {
            program.bufferElementCoreTypes[binding] =
                static_cast<u8>(coreType);
          }
        }
      }

      u16 storedInitReg = ConvertRegisterToType(initReg, coreType);
      builder.EmitInstruction(OP_STORE_REG, varReg, storedInitReg);
      // Propagate pointer storage info if the init expression is a pointer
      if (initReg < MAX_REGISTERS && (program.registerStorageInfo[initReg] &
                                      IR::IRProgram::STORAGE_IS_PTR)) {
        program.registerStorageInfo[varReg] =
            program.registerStorageInfo[initReg];
        // Also propagate the struct type hash for `TypeName^ p = ^s;` so
        // `p^.field` later resolves the struct layout.
        if (program.registerStructTypes[initReg] != 0 &&
            varReg < MAX_REGISTERS) {
          program.registerStructTypes[varReg] =
              program.registerStructTypes[initReg];
        }
      }
      initializedVariables.insert(varDecl.name.nameHash);
    }
  } else if (!isArray && coreType == CoreType::CUSTOM) {
    // For single struct types without initializer, emit a zero/undef struct
    // value. Note: struct arrays are handled differently - they are
    // pointer-based and individual elements are stored via ARRAY_STORE.
    u32 structTypeHash = variableStructTypes[varDecl.name.nameHash];
    if (structTypeHash == 0 && varReg < MAX_REGISTERS) {
      structTypeHash = program.registerStructTypes[varReg];
    }
    if (structTypeHash != 0) {
      u16 zeroStruct = EmitZeroStruct(structTypeHash);
      builder.EmitInstruction(OP_STORE_REG, varReg, zeroStruct);
      initializedVariables.insert(varDecl.name.nameHash);
    }
  } else if (!isArray &&
             (coreType == CoreType::FLOAT2 || coreType == CoreType::FLOAT3 ||
              coreType == CoreType::FLOAT4)) {
    // For vector types without initializer, emit a zero vector construct
    // This ensures the variable has a defined value for OpCompositeInsert to
    // use
    u16 zero = builder.EmitConstant(0.0f);
    u32 componentCount = (coreType == CoreType::FLOAT2)   ? 2
                         : (coreType == CoreType::FLOAT3) ? 3
                                                          : 4;
    builder.EmitInstruction(OP_VEC_CONSTRUCT, varReg, zero, zero, zero, zero);
    program.metadata[program.instructionCount - 1] = componentCount;
    initializedVariables.insert(varDecl.name.nameHash);
  }
}

inline void IRLowering::LowerAssignment(NodeRef ref) {
  const AssignmentData &assign = ast->GetAssignment(ref);
  u16 valueReg = LowerExpression(assign.value);

  NodeRef target = assign.target;

  if (target.Type() == ASTNodeType::IDENTIFIER) {
    const IdentifierData &ident = ast->GetIdentifier(target);
    if (currentStructMethodTypeHash != 0 &&
        currentStructMethodSelfReg != 0xFFFF &&
        variableRegisters.find(ident.name.nameHash) == variableRegisters.end()) {
      u32 fieldIndex = 0xFFFFFFFF;
      CoreType fieldType = CoreType::INVALID;
      if (FindStructField(currentStructMethodTypeHash, ident.name.nameHash,
                          &fieldIndex, &fieldType, nullptr)) {
        if (currentStructMethodIsConst) {
          ReportError("Error: cannot assign to receiver field inside const method\n");
          return;
        }
        valueReg = ConvertRegisterToType(valueReg, fieldType);
        u16 newStructReg = AllocateRegister();
        SetRegisterType(newStructReg, CoreType::CUSTOM);
        program.registerStructTypes[newStructReg] = currentStructMethodTypeHash;
        builder.EmitInstruction(OP_STRUCT_INSERT, newStructReg,
                                currentStructMethodSelfReg, fieldIndex,
                                valueReg);
        program.metadata[builder.currentInstruction - 1] =
            currentStructMethodTypeHash;
        builder.EmitInstruction(OP_STORE_REG, currentStructMethodSelfReg,
                                newStructReg);
        return;
      }
    }

    u16 varReg = GetOrAllocateVariable(ident.name.nameHash);
    valueReg = ConvertRegisterToType(valueReg, GetRegisterType(varReg));
    builder.EmitInstruction(OP_STORE_REG, varReg, valueReg);
    // Propagate pointer storage info if the value is a pointer
    if (valueReg < MAX_REGISTERS && (program.registerStorageInfo[valueReg] &
                                     IR::IRProgram::STORAGE_IS_PTR)) {
      program.registerStorageInfo[varReg] =
          program.registerStorageInfo[valueReg];
    }
    initializedVariables.insert(ident.name.nameHash);
  } else if (target.Type() == ASTNodeType::MEMBER_ACCESS) {
    const MemberAccessData &access = ast->GetMemberAccess(target);

    if (access.object.Type() == ASTNodeType::MEMBER_ACCESS) {
      const MemberAccessData &baseAccess =
          ast->GetMemberAccess(access.object);
      if (baseAccess.object.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData &baseObj = ast->GetIdentifier(baseAccess.object);
        if (baseObj.identifierKind == SpecialIdentifier::OUTPUT) {
          u32 outputNameHash = baseAccess.member.nameHash;
          CoreType outputType = ResolveOutputTypeForLoad(outputNameHash);

          const char *nameStr = nullptr;
          char nameBuf[32] = {0};
          if (sourceBase && !baseAccess.member.isHashOnly()) {
            auto sv = baseAccess.member.view(sourceBase);
            size_t len = sv.length() < 31 ? sv.length() : 31;
            memcpy(nameBuf, sv.data(), len);
            nameBuf[len] = '\0';
            nameStr = nameBuf;
          }
          u32 slot = ResolveOutputSlotForStore(outputNameHash, outputType,
                                               nameStr, assign.interpolation);

          u16 outputReg = AllocateRegister();
          SetRegisterType(outputReg, outputType);
          builder.EmitInstruction(OP_LOAD_OUTPUT, outputReg, slot);
          program.metadata[builder.currentInstruction - 1] = outputNameHash;

          u32 memberHash = access.member.nameHash;
          u32 componentIndex = 0xFFFFFFFF;
          if (memberHash == Utils::HashStr("x") ||
              memberHash == Utils::HashStr("r"))
            componentIndex = 0;
          else if (memberHash == Utils::HashStr("y") ||
                   memberHash == Utils::HashStr("g"))
            componentIndex = 1;
          else if (memberHash == Utils::HashStr("z") ||
                   memberHash == Utils::HashStr("b"))
            componentIndex = 2;
          else if (memberHash == Utils::HashStr("w") ||
                   memberHash == Utils::HashStr("a"))
            componentIndex = 3;

          if (componentIndex != 0xFFFFFFFF) {
            u32 numComponents = GetVectorDimension(outputType);
            if (numComponents < 2) {
              builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
              program.metadata[builder.currentInstruction - 1] =
                  outputNameHash;
              return;
            }

            u16 newVecReg = AllocateRegister();
            SetRegisterType(newVecReg, outputType);
            builder.EmitInstruction(OP_VEC_INSERT, newVecReg, outputReg,
                                    componentIndex, valueReg);
            builder.EmitInstruction(OP_STORE_OUTPUT, newVecReg, slot);
            program.metadata[builder.currentInstruction - 1] = outputNameHash;
            return;
          }

          u8 swizzleIndices[4] = {0, 0, 0, 0};
          u32 swizzleLen = 0;
          if (DecodeSwizzleByHash(memberHash, access.member.nameLength,
                                  swizzleIndices, &swizzleLen)) {
            u32 numComponents = GetVectorDimension(outputType);
            if (numComponents < 2) {
              builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
              program.metadata[builder.currentInstruction - 1] =
                  outputNameHash;
              return;
            }

            u16 newVecReg = AllocateRegister();
            SetRegisterType(newVecReg, outputType);

            u32 shuffleMask = 0;
            for (u32 j = 0; j < numComponents; j++) {
              u8 fromValue = 255;
              for (u32 k = 0; k < swizzleLen; k++) {
                if (swizzleIndices[k] == j) {
                  fromValue = static_cast<u8>(k);
                }
              }
              u8 srcIdx = (fromValue != 255)
                              ? static_cast<u8>(fromValue + numComponents)
                              : static_cast<u8>(j);
              shuffleMask |= (srcIdx & 0xF) << (j * 4);
            }

            builder.EmitInstruction(OP_VEC_SHUFFLE, newVecReg, outputReg,
                                    valueReg);
            program.metadata[builder.currentInstruction - 1] = shuffleMask;
            builder.EmitInstruction(OP_STORE_OUTPUT, newVecReg, slot);
            program.metadata[builder.currentInstruction - 1] =
                outputNameHash;
            return;
          }
        }
      }
    }

    // Check if object is 'output'
    if (access.object.Type() == ASTNodeType::IDENTIFIER) {
      const IdentifierData &obj = ast->GetIdentifier(access.object);
      if (obj.identifierKind == SpecialIdentifier::OUTPUT) {
        u32 nameHash = access.member.nameHash;
        CoreType valueType = GetRegisterType(valueReg);

        // Extract name string for GLES output
        const char *nameStr = nullptr;
        char nameBuf[32] = {0};
        if (sourceBase && !access.member.isHashOnly()) {
          auto sv = access.member.view(sourceBase);
          size_t len = sv.length() < 31 ? sv.length() : 31;
          memcpy(nameBuf, sv.data(), len);
          nameBuf[len] = '\0';
          nameStr = nameBuf;
        }
        u32 slot = ResolveOutputSlotForStore(nameHash, valueType, nameStr,
                                             assign.interpolation);
        CoreType declaredOutputType = GetFragmentOutputType(nameHash);
        if (currentStage == ShaderStage::Fragment &&
            declaredOutputType != CoreType::INVALID &&
            valueType != declaredOutputType) {
          ReportError("Error: fragment output assignment type does not match outputs declaration\n");
        }

        // Emit OP_STORE_OUTPUT with slot in operand
        builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
        program.metadata[builder.currentInstruction - 1] =
            nameHash; // Keep name for debugging
      } else if (obj.identifierKind == SpecialIdentifier::NONE ||
                 obj.identifierKind == SpecialIdentifier::SELF) {
        // Struct member assignment: s.field = value
        if (currentStructMethodIsConst &&
            obj.name.nameHash == Utils::HashStr("self")) {
          ReportError("Error: cannot assign to receiver field inside const method\n");
          return;
        }

        u16 objReg = GetOrAllocateVariable(obj.name.nameHash);

        // Look up the variable's struct type
        u32 structTypeHash = 0;
        auto varIt = variableStructTypes.find(obj.name.nameHash);
        if (varIt != variableStructTypes.end()) {
          structTypeHash = varIt->second;
        } else if (objReg < MAX_REGISTERS) {
          structTypeHash = program.registerStructTypes[objReg];
        }

        if (structTypeHash != 0) {
          if (initializedVariables.find(obj.name.nameHash) ==
              initializedVariables.end()) {
            u16 zeroStruct = EmitZeroStruct(structTypeHash);
            builder.EmitInstruction(OP_STORE_REG, objReg, zeroStruct);
            initializedVariables.insert(obj.name.nameHash);
          }

          // Find the field index
          u32 fieldIndex = 0xFFFFFFFF;
          auto structIt = structTypeMap.find(structTypeHash);
          if (structIt != structTypeMap.end()) {
            u32 structIdx = structIt->second;
            const IRProgram::StructTypeInfo &info =
                program.structTypes[structIdx];
            for (u32 i = 0; i < info.fieldCount; i++) {
              if (program.structFieldNameHashes[info.fieldOffset + i] ==
                  access.member.nameHash) {
                fieldIndex = i;
                break;
              }
            }
          }

          if (fieldIndex != 0xFFFFFFFF) {
            // Emit struct insert: dest = struct with field=value
            // We modify the struct in place by creating a new struct value
            u16 newStructReg = AllocateRegister();
            SetRegisterType(newStructReg, CoreType::CUSTOM);
            program.registerStructTypes[newStructReg] = structTypeHash;
            builder.EmitInstruction(OP_STRUCT_INSERT, newStructReg, objReg,
                                    fieldIndex, valueReg);
            // Store full struct type hash in metadata (field index is in
            // operand 1)
            program.metadata[builder.currentInstruction - 1] = structTypeHash;

            // Copy the new struct value back to the variable
            builder.EmitInstruction(OP_STORE_REG, objReg, newStructReg);

            // Update struct type for the register
            program.registerStructTypes[newStructReg] = structTypeHash;
            return;
          }
        }

        // Check for vector component assignment (pos.x = value, etc.)
        u32 componentIndex = 0xFFFFFFFF;
        u32 memberHash = access.member.nameHash;
        if (memberHash == Utils::HashStr("x") ||
            memberHash == Utils::HashStr("r"))
          componentIndex = 0;
        else if (memberHash == Utils::HashStr("y") ||
                 memberHash == Utils::HashStr("g"))
          componentIndex = 1;
        else if (memberHash == Utils::HashStr("z") ||
                 memberHash == Utils::HashStr("b"))
          componentIndex = 2;
        else if (memberHash == Utils::HashStr("w") ||
                 memberHash == Utils::HashStr("a"))
          componentIndex = 3;

        if (componentIndex != 0xFFFFFFFF) {
          // Vector component assignment: vec.x = value
          // We need to insert the component into the vector and store back
          CoreType varType = GetRegisterType(objReg);

          // Create a new vector with the component inserted
          u16 newVecReg = AllocateRegister();
          SetRegisterType(newVecReg, varType);

          // OP_VEC_INSERT: dest = insert(src_vec, component_index, value)
          builder.EmitInstruction(OP_VEC_INSERT, newVecReg, objReg,
                                  componentIndex, valueReg);

          // Copy the new vector back to the variable
          builder.EmitInstruction(OP_STORE_REG, objReg, newVecReg);
          return;
        }

        // Multi-component swizzle assignment. Parse the member name
        // character-by-character into output-position -> source-index
        // mapping, then build a single OpVectorShuffle picking from
        // value for written positions and from the original otherwise.
        // Handles in-order (xy, xyz), out-of-order (wz, yx, xzy) and
        // repeated components across either xyzw or rgba sets.
        {
          u8 targetIdx[4] = {0, 0, 0, 0};
          u32 swizzleLen = 0;
          if (DecodeSwizzleByHash(access.member.nameHash,
                                  access.member.nameLength, targetIdx,
                                  &swizzleLen)) {
            CoreType varType = GetRegisterType(objReg);
            u32 numComponents = GetVectorDimension(varType);
            if (numComponents < 2) {
              builder.EmitInstruction(OP_STORE_REG, objReg, valueReg);
              return;
            }

            // inverse map: for each original-vec position j,
            // which value-vec component (if any) overwrites it?
            u8 fromValue[4] = {255, 255, 255, 255};
            for (u32 i = 0; i < swizzleLen; i++) {
              u8 origSlot = targetIdx[i];
              if (origSlot < numComponents) {
                fromValue[origSlot] = static_cast<u8>(i);
              }
            }

            u32 shuffleMask = 0;
            for (u32 j = 0; j < numComponents; j++) {
              if (fromValue[j] != 255) {
                shuffleMask |=
                    ((fromValue[j] + numComponents) & 0xF) << (j * 4);
              } else {
                shuffleMask |= (j & 0xF) << (j * 4);
              }
            }

            u16 newVecReg = AllocateRegister();
            SetRegisterType(newVecReg, varType);
            builder.EmitInstruction(OP_VEC_SHUFFLE, newVecReg, objReg,
                                    valueReg);
            program.metadata[builder.currentInstruction - 1] = shuffleMask;

            builder.EmitInstruction(OP_STORE_REG, objReg, newVecReg);
            return;
          }
        }

        // Fallback: unknown struct/field - just store directly
        builder.EmitInstruction(OP_STORE_REG, objReg, valueReg);
      }
    } else if (access.object.Type() == ASTNodeType::ARRAY_ACCESS) {
      // Struct array element field assignment: arr[i].field = value
      // e.g., lights[0].position = float3(10, 10, 10)
      const ArrayAccessData &arrAccess = ast->GetArrayAccess(access.object);
      u16 arrayReg = LowerExpression(arrAccess.array);
      u16 indexReg = LowerExpression(arrAccess.index);

      // Get the struct type for the array elements
      u32 structTypeHash = 0;
      if (arrayReg < MAX_REGISTERS) {
        structTypeHash = program.registerStructTypes[arrayReg];
      }
      // Check localArrayStructTypes if registerStructTypes doesn't have it
      if (structTypeHash == 0 && program.registerStorageInfo &&
          arrayReg < program.registerCount &&
          (program.registerStorageInfo[arrayReg] &
           IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) {
        u32 arrayIdx = (program.registerStorageInfo[arrayReg] >>
                        IR::IRProgram::STORAGE_BINDING_SHIFT);
        if (arrayIdx < program.localArrayCount &&
            program.localArrayStructTypes) {
          structTypeHash = program.localArrayStructTypes[arrayIdx];
        }
      }

      if (structTypeHash != 0) {
        // Find the field index
        u32 fieldIndex = 0xFFFFFFFF;
        auto structIt = structTypeMap.find(structTypeHash);
        if (structIt != structTypeMap.end()) {
          u32 structIdx = structIt->second;
          const IRProgram::StructTypeInfo &info =
              program.structTypes[structIdx];
          for (u32 i = 0; i < info.fieldCount; i++) {
            if (program.structFieldNameHashes[info.fieldOffset + i] ==
                access.member.nameHash) {
              fieldIndex = i;
              break;
            }
          }
        }

        if (fieldIndex != 0xFFFFFFFF) {
          // 1. Load the struct element from the array
          u16 elemReg = AllocateRegister();
          SetRegisterType(elemReg, CoreType::CUSTOM);
          program.registerStructTypes[elemReg] = structTypeHash;
          builder.EmitInstruction(OP_ARRAY_LOAD, elemReg, arrayReg, indexReg);

          // 2. Insert the new field value into the struct
          u16 newStructReg = AllocateRegister();
          SetRegisterType(newStructReg, CoreType::CUSTOM);
          program.registerStructTypes[newStructReg] = structTypeHash;
          builder.EmitInstruction(OP_STRUCT_INSERT, newStructReg, elemReg,
                                  fieldIndex, valueReg);
          program.metadata[builder.currentInstruction - 1] = structTypeHash;

          // 3. Store the modified struct back to the array
          builder.EmitInstruction(OP_ARRAY_STORE, arrayReg, indexReg,
                                  newStructReg);
          return;
        }
      }
    }
  } else if (target.Type() == ASTNodeType::ARRAY_ACCESS) {
    const ArrayAccessData &arrAccess = ast->GetArrayAccess(target);
    u16 baseReg = LowerExpression(arrAccess.array);
    u16 indexReg = LowerExpression(arrAccess.index);
    // `v[i] = x` where `v` is a scalar vector local (not `v` declared as
    // `float3[N]`) must go through OpVectorInsertDynamic —
    // OpCompositeInsert can't take a runtime literal index. Distinguish
    // the vector-component case from the array-of-vectors case by
    // comparing the value's type against the base's type:
    //   - vec-component write: base is a vector, value is the scalar
    //     component type (e.g., base=float3, value=float)
    //   - array-of-vectors element write: base tracked as FLOAT3 but
    //     the value is the whole element (float3)
    if (baseReg < MAX_REGISTERS) {
      CoreType baseType =
          static_cast<CoreType>(program.registerTypes[baseReg]);
      CoreType valueType = GetRegisterType(valueReg);
      TypeMask btm = mask(baseType);
      bool baseIsVec = btm & (TypeMasks::FLOAT_VECTORS | TypeMasks::INT_VECTORS |
                               TypeMasks::UINT_VECTORS | TypeMasks::BOOL_VECTORS);
      bool valueIsComponent =
          (baseType == CoreType::FLOAT2 || baseType == CoreType::FLOAT3 ||
           baseType == CoreType::FLOAT4) ? (valueType == CoreType::FLOAT) :
          (baseType == CoreType::INT2   || baseType == CoreType::INT3 ||
           baseType == CoreType::INT4)  ? (valueType == CoreType::INT) :
          (baseType == CoreType::UINT2  || baseType == CoreType::UINT3 ||
           baseType == CoreType::UINT4) ? (valueType == CoreType::UINT) :
          (baseType == CoreType::BOOL2  || baseType == CoreType::BOOL3 ||
           baseType == CoreType::BOOL4) ? (valueType == CoreType::BOOL) : false;
      if (baseIsVec && valueIsComponent) {
        u16 newVecReg = AllocateRegister();
        SetRegisterType(newVecReg, baseType);
        builder.EmitInstruction(OP_VEC_INSERT_DYNAMIC, newVecReg, baseReg,
                                valueReg, indexReg);
        builder.EmitInstruction(OP_STORE_REG, baseReg, newVecReg);
        return;
      }

      // Matrix-column write: `M[i] = col_vec` where M is a matrix-typed
      // local and i is a compile-time integer. Emit OP_VEC_INSERT
      // (OpCompositeInsert) so the matrix type is preserved — the
      // generic ARRAY_STORE fallback aliases baseReg to value, turning
      // the matrix into a column-vec and breaking subsequent reads.
      //
      // Only fire when the LHS is `identifier[i]` — a simple local
      // matrix variable. If the LHS is `struct.field[i]` (array-of-
      // matrix field), the CoreType is wrong for matrix semantics
      // (we have no "array of mat4" CoreType, so the register may
      // carry mat4 spuriously). Fall through to generic ARRAY_STORE
      // so STRUCT_INSERT can reconstitute the struct correctly.
      bool baseIsLocalMatrix =
          arrAccess.array.Type() == ASTNodeType::IDENTIFIER;
      bool baseIsMat =
          baseType == CoreType::MAT2 || baseType == CoreType::MAT3 ||
          baseType == CoreType::MAT4;
      CoreType expectedCol = (baseType == CoreType::MAT4)   ? CoreType::FLOAT4
                           : (baseType == CoreType::MAT3)   ? CoreType::FLOAT3
                           : (baseType == CoreType::MAT2)   ? CoreType::FLOAT2
                                                            : CoreType::INVALID;
      bool indexIsConst = (indexReg & 0x4000) != 0;
      if (baseIsLocalMatrix && baseIsMat && indexIsConst &&
          valueType == expectedCol) {
        u16 slot = indexReg & 0x3FFF;
        u16 literalIdx = 0;
        if (slot < program.intCount) {
          literalIdx = static_cast<u16>(program.intConstants[slot]);
        }
        u16 newMatReg = AllocateRegister();
        SetRegisterType(newMatReg, baseType);
        builder.EmitInstruction(OP_VEC_INSERT, newMatReg, baseReg,
                                literalIdx, valueReg);
        builder.EmitInstruction(OP_STORE_REG, baseReg, newMatReg);
        return;
      }
    }
    if (baseReg < MAX_REGISTERS) {
      CoreType baseType =
          static_cast<CoreType>(program.registerTypes[baseReg]);
      CoreType valueType = GetRegisterType(valueReg);
      if (valueType != CoreType::INVALID && valueType != CoreType::VOID) {
        if (baseType == CoreType::INVALID || baseType == CoreType::VOID ||
            baseType != valueType) {
          SetRegisterType(baseReg, valueType);
        }
        if ((valueType == CoreType::CUSTOM || valueType == CoreType::ENUM) &&
            valueReg < MAX_REGISTERS) {
          u32 structHash = program.registerStructTypes[valueReg];
          if (structHash != 0) {
            program.registerStructTypes[baseReg] = structHash;
          }
        }
      }
    }
    builder.EmitInstruction(OP_ARRAY_STORE, baseReg, indexReg, valueReg);
  } else if (target.Type() == ASTNodeType::UNARY_OP) {
    // Handle dereference as lvalue: ptr^ = value
    const UnaryOpData &unaryOp = ast->GetUnaryOp(target);
    if (unaryOp.op == UnaryOpType::DEREFERENCE) {
      // Get the pointer value
      u16 ptrReg = LowerExpression(unaryOp.operand);
      // Store through the pointer
      // Format: dest=0 (no result), s0=ptr, s1=value
      builder.EmitInstruction(OP_LOCAL_STORE, 0, ptrReg, valueReg);
    }
  }
}

inline u16 IRLowering::LowerArrayAccess(NodeRef ref) {
  const ArrayAccessData &access = ast->GetArrayAccess(ref);

  u16 baseReg = LowerExpression(access.array);
  u16 indexReg = LowerExpression(access.index);
  u16 dest = AllocateRegister();

  // Check if the base register is a storage buffer pointer (but NOT a local
  // array)
  bool isStoragePtr = baseReg < MAX_REGISTERS &&
                      (program.registerStorageInfo[baseReg] &
                       IR::IRProgram::STORAGE_IS_PTR) &&
                      !(program.registerStorageInfo[baseReg] &
                        IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

  if (isStoragePtr) {
    // Storage buffer array indexing:
    // 1. Emit OP_STORAGE_INDEX to compute the element pointer
    // 2. Emit OP_STORAGE_LOAD to load the value

    u16 ptrReg = AllocateRegister();
    builder.EmitInstruction(OP_STORAGE_INDEX, ptrReg, baseReg, indexReg);

    // Propagate storage info for the pointer (in case of further chaining)
    u32 srcInfo = program.registerStorageInfo[baseReg];
    u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
    u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
                 IR::IRProgram::STORAGE_DEPTH_SHIFT) +
                1;
    u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
    program.registerStorageInfo[ptrReg] =
        (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
        (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
        IR::IRProgram::STORAGE_IS_PTR | sharedFlag;

    CoreType baseType = GetRegisterType(baseReg);
    if (baseType != CoreType::INVALID && baseType != CoreType::VOID) {
      SetRegisterType(ptrReg, baseType);
    }

    // Now load the value from the element pointer
    builder.EmitInstruction(OP_STORAGE_LOAD, dest, ptrReg);

    // Store the binding info in dest for later struct type association
    // (without IS_PTR flag since this is a loaded value, not a pointer)
    if (dest < MAX_REGISTERS) {
      program.registerStorageInfo[dest] =
          (binding << IR::IRProgram::STORAGE_BINDING_SHIFT);
    }

    // Propagate element type from base array type
    if (baseReg < MAX_REGISTERS) {
      CoreType baseType =
          static_cast<CoreType>(program.registerTypes[baseReg]);
      SetRegisterType(dest, baseType);
      if ((baseType == CoreType::CUSTOM || baseType == CoreType::ENUM) &&
          baseReg < MAX_REGISTERS) {
        u32 srcHash = program.registerStructTypes[baseReg];
        if (srcHash != 0) {
          // Ensure the struct type is registered in structTypeMap
          u32 canonicalHash = LookupOrRegisterStructType(srcHash);
          program.registerStructTypes[dest] =
              canonicalHash != 0 ? canonicalHash : srcHash;
        }
      }
    }
  } else {
    // Regular array load
    builder.EmitInstruction(OP_ARRAY_LOAD, dest, baseReg, indexReg);

    // Compute correct element type based on base type
    if (baseReg < MAX_REGISTERS) {
      CoreType baseType =
          static_cast<CoreType>(program.registerTypes[baseReg]);
      CoreType elemType = baseType;

      // Check if this is a local array (subscripting returns element, not
      // scalar)
      bool isLocalArray = (program.registerStorageInfo[baseReg] &
                           IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

      if (!isLocalArray) {
        // Matrix subscript returns a column vector
        if (baseType == CoreType::MAT2)
          elemType = CoreType::FLOAT2;
        else if (baseType == CoreType::MAT3)
          elemType = CoreType::FLOAT3;
        else if (baseType == CoreType::MAT4)
          elemType = CoreType::FLOAT4;
        // Vector subscript returns a scalar (but NOT for local arrays of
        // vectors)
        else if (baseType == CoreType::FLOAT2 ||
                 baseType == CoreType::FLOAT3 || baseType == CoreType::FLOAT4)
          elemType = CoreType::FLOAT;
        else if (baseType == CoreType::INT2 || baseType == CoreType::INT3 ||
                 baseType == CoreType::INT4)
          elemType = CoreType::INT;
        else if (baseType == CoreType::UINT2 || baseType == CoreType::UINT3 ||
                 baseType == CoreType::UINT4)
          elemType = CoreType::UINT;
      }
      // For local arrays, elemType stays as baseType (the array element type)

      SetRegisterType(dest, elemType);
      if ((baseType == CoreType::CUSTOM || baseType == CoreType::ENUM) &&
          baseReg < MAX_REGISTERS) {
        u32 srcHash = program.registerStructTypes[baseReg];
        if (srcHash != 0) {
          // Ensure the struct type is registered in structTypeMap
          u32 canonicalHash = LookupOrRegisterStructType(srcHash);
          program.registerStructTypes[dest] =
              canonicalHash != 0 ? canonicalHash : srcHash;
        }
      }
    }
  }

  return dest;
}

inline u16 IRLowering::LowerStoragePointerForAtomic(NodeRef ref) {
  if (ref.Type() != ASTNodeType::ARRAY_ACCESS) {
    return LowerExpression(ref);
  }

  const ArrayAccessData &access = ast->GetArrayAccess(ref);

  u16 baseReg = LowerExpression(access.array);
  u16 indexReg = LowerExpression(access.index);

  bool isStoragePtr =
      baseReg < MAX_REGISTERS &&
      (program.registerStorageInfo[baseReg] & IR::IRProgram::STORAGE_IS_PTR);

  if (!isStoragePtr && access.array.Type() == ASTNodeType::IDENTIFIER) {
    for (u32 i = 0; i < program.sharedVarCount; i++) {
      if (program.sharedRegisters[i] == baseReg) {
        program.registerStorageInfo[baseReg] =
            (i << IR::IRProgram::STORAGE_BINDING_SHIFT) |
            IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_SHARED;
        isStoragePtr = true;
        break;
      }
    }
  }

  if (!isStoragePtr) {
    return LowerExpression(ref);
  }

  u16 ptrReg = AllocateRegister();
  builder.EmitInstruction(OP_STORAGE_INDEX, ptrReg, baseReg, indexReg);

  u32 srcInfo = program.registerStorageInfo[baseReg];
  u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
  u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
               IR::IRProgram::STORAGE_DEPTH_SHIFT) +
              1;
  u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
  program.registerStorageInfo[ptrReg] =
      (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
      (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
      IR::IRProgram::STORAGE_IS_PTR | sharedFlag;

  CoreType baseType = GetRegisterType(baseReg);
  if (baseType != CoreType::INVALID && baseType != CoreType::VOID) {
    SetRegisterType(ptrReg, baseType);
  }

  return ptrReg;
}

inline u16 IRLowering::LowerMemberAccess(NodeRef ref) {
  const MemberAccessData &access = ast->GetMemberAccess(ref);
  static const u32 HASH_LENGTH = Utils::HashStr("length");

  if (access.member.nameHash == HASH_LENGTH) {
    u32 arrayLength = ResolveStaticArrayLength(this, access.object);
    if (arrayLength > 0) {
      return EmitConstantInt(arrayLength);
    }
  }

  // Sum-type enum variant with no payload, e.g. `Curve::Linear`.
  // Payload variants with arguments lower through LowerFunctionCall.
  // Any remaining module-qualified expression must resolve here; otherwise
  // unknown `Module::member` expressions fall through as struct accesses.
  if (access.isModuleQualified &&
      access.object.Type() == ASTNodeType::IDENTIFIER) {
    const IdentifierData &enumIdent = ast->GetIdentifier(access.object);
    u32 qualifierHash = enumIdent.name.nameHash;
    Symbol *enumSym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), qualifierHash);
    if (enumSym && (enumSym->kind == SymbolKind::ENUM ||
                    enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
      const EnumData &enumData = symbols->enums[enumSym->index];
      if (enumData.flags & EnumData::IS_SUM_TYPE) {
        for (u32 v = 0; v < enumData.variants.count; v++) {
          const EnumData::Variant &variant = enumData.variants[v];
          if (variant.name.nameHash == access.member.nameHash &&
              variant.associatedTypes.count == 0) {
            u16 dest = AllocateRegister();
            u32 enumStructHash = LookupOrRegisterStructType(qualifierHash);
            builder.EmitInstruction(OP_ENUM_CONSTRUCT, dest, 0x3FFF,
                                    0x3FFF, 0x3FFF, 0x3FFF);
            program.metadata[builder.currentInstruction - 1] =
                (v << 16) | (0u << 8) | (qualifierHash & 0xFF);
            SetRegisterType(dest, CoreType::CUSTOM);
            if (dest < MAX_REGISTERS) {
              program.registerStructTypes[dest] = enumStructHash;
            }
            return dest;
          }
        }
      }
      std::string message = "Error: Unknown enum variant: " +
                            ReverseLookup::GetString(qualifierHash) + "::" +
                            ReverseLookup::GetString(access.member.nameHash) +
                            "\n";
      ReportError(message.c_str());
      return 0;
    }

    u32 moduleIndex = SymbolTable::ResolveModuleIndexByHashInScope(
        const_cast<SymbolTableData *>(symbols), qualifierHash,
        AliasOwnerKind(), AliasOwnerModuleIndex());
    if (moduleIndex == INVALID_INDEX) {
      std::string message = "Error: Unknown module or enum: " +
                            ReverseLookup::GetString(qualifierHash) + "\n";
      ReportError(message.c_str());
      return 0;
    }
    std::string message = "Error: Unknown module member: " +
                          ReverseLookup::GetString(qualifierHash) + "::" +
                          ReverseLookup::GetString(access.member.nameHash) +
                          "\n";
    ReportError(message.c_str());
    return 0;
  }

  // Resolve `variants.<name>` to the variant's current value as a
  // constant. Raster stages (vertex/fragment) go through
  // CloneShaderStageWithParams which substitutes variants at clone time,
  // but compute / direct stages reach IR lowering with the AST still
  // containing MemberAccess(VARIANTS, name). Without this, the default
  // MemberAccess path treats `variants` as a struct-valued register and
  // emits a bogus OpCompositeExtract on a scalar constant.
  if (access.object.Type() == ASTNodeType::IDENTIFIER &&
      !currentPipeline.IsNull()) {
    const IdentifierData &objIdent = ast->GetIdentifier(access.object);
    if (objIdent.identifierKind == SpecialIdentifier::VARIANTS) {
      const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
      for (u32 i = 0; i < pipeline.variantDecls.count; i++) {
        const PipelineVariantDeclData &decl = pipeline.variantDecls[i];
        if (decl.name.nameHash == access.member.nameHash && decl.defaultResolved) {
          switch (decl.defaultValue.type) {
            case LiteralValue::FLOAT:
              return builder.EmitConstant(decl.defaultValue.floatValue);
            case LiteralValue::INT:
              return EmitConstantInt(decl.defaultValue.intValue);
            case LiteralValue::UINT:
              return EmitConstantUint(decl.defaultValue.uintValue);
            case LiteralValue::BOOL:
              return builder.EmitConstantBool(decl.defaultValue.boolValue);
            default:
              break;
          }
        }
      }
    }
  }

  // Handle chained member access (e.g., attributes.normal.y or
  // resources.lights.positions)
  if (access.object.Type() == ASTNodeType::MEMBER_ACCESS ||
      access.object.Type() == ASTNodeType::ARRAY_ACCESS) {
    // First, lower the object (e.g., attributes.normal -> float3 register)
    u16 objectReg = LowerExpression(access.object);

    // Check if the object register has a struct type (e.g., resources.lights
    // -> LightSourcesSoA)
    u32 structTypeHash = 0;
    if (objectReg < MAX_REGISTERS) {
      structTypeHash = program.registerStructTypes[objectReg];
    }

    if (structTypeHash != 0) {
      // This is a struct field access (e.g., lights.positions)
      u32 fieldIndex = 0xFFFFFFFF;
      CoreType fieldType = CoreType::FLOAT;
      u32 fieldOffset = 0;

      auto structIt = structTypeMap.find(structTypeHash);

      if (structIt != structTypeMap.end()) {
        u32 structIdx = structIt->second;
        const IRProgram::StructTypeInfo &info =
            program.structTypes[structIdx];
        fieldOffset = info.fieldOffset;
        for (u32 i = 0; i < info.fieldCount; i++) {
          if (program.structFieldNameHashes[info.fieldOffset + i] ==
              access.member.nameHash) {
            fieldIndex = i;
            fieldType = static_cast<CoreType>(
                program.structFieldTypes[info.fieldOffset + i]);
            break;
          }
        }
      }

      if (fieldIndex != 0xFFFFFFFF) {
        u16 dest = AllocateRegister();

        // Check if the object register is a storage buffer pointer
        bool isStoragePtr = objectReg < MAX_REGISTERS &&
                            (program.registerStorageInfo[objectReg] &
                             IR::IRProgram::STORAGE_IS_PTR);

        if (isStoragePtr) {
          // Storage buffer field access - emit OP_STORAGE_FIELD to maintain
          // pointer semantics
          builder.EmitInstruction(OP_STORAGE_FIELD, dest, objectReg,
                                  fieldIndex);
          program.metadata[builder.currentInstruction - 1] = structTypeHash;

          // Propagate storage pointer info (increment depth)
          u32 srcInfo = program.registerStorageInfo[objectReg];
          u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
          u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
                       IR::IRProgram::STORAGE_DEPTH_SHIFT) +
                      1;
          u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
          program.registerStorageInfo[dest] =
              (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
              (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
              IR::IRProgram::STORAGE_IS_PTR | sharedFlag;
        } else {
          // Regular struct field access - emit OP_STRUCT_EXTRACT
          builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objectReg,
                                  fieldIndex);
          program.metadata[builder.currentInstruction - 1] = structTypeHash;
        }

        SetRegisterType(dest, fieldType);
        if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
            program.structFieldTypeHashes) {
          u32 fieldTypeHash =
              program.structFieldTypeHashes[fieldOffset + fieldIndex];
          if (fieldTypeHash != 0 && dest < MAX_REGISTERS) {
            program.registerStructTypes[dest] = fieldTypeHash;
          }
        }
        return dest;
      }
    }

    // Then extract the component based on the member name (x/y/z/w or
    // r/g/b/a)
    u32 memberHash = access.member.nameHash;
    u32 componentIndex = 0xFFFFFFFF;

    // Check for xyzw swizzle
    if (memberHash == Utils::HashStr("x") ||
        memberHash == Utils::HashStr("r"))
      componentIndex = 0;
    else if (memberHash == Utils::HashStr("y") ||
             memberHash == Utils::HashStr("g"))
      componentIndex = 1;
    else if (memberHash == Utils::HashStr("z") ||
             memberHash == Utils::HashStr("b"))
      componentIndex = 2;
    else if (memberHash == Utils::HashStr("w") ||
             memberHash == Utils::HashStr("a"))
      componentIndex = 3;

    if (componentIndex != 0xFFFFFFFF) {
      // If objectReg is a storage pointer, load it first before swizzle
      u16 srcReg = objectReg;
      if (objectReg < MAX_REGISTERS &&
          (program.registerStorageInfo[objectReg] &
           IR::IRProgram::STORAGE_IS_PTR)) {
        srcReg = AllocateRegister();
        CoreType loadType = GetRegisterType(objectReg);
        SetRegisterType(srcReg, loadType);
        builder.EmitInstruction(OP_STORAGE_LOAD, srcReg, objectReg);
      }
      // `.x` / `.r` on an already-scalar value is a no-op (e.g. the
      // inner `.r` of `v.r.r`). Emitting OP_VEC_EXTRACT on a scalar
      // makes the SPIR-V backend synthesise OpCompositeExtract on a
      // non-composite and trips the validator. Skip when the object
      // came from an ARRAY_ACCESS — array loads of struct-array
      // fields carry an IR-level scalar CoreType but a SPIR-V
      // vector type override, so the guard would incorrectly
      // suppress a needed extract.
      CoreType srcType = GetRegisterType(srcReg);
      bool srcFromArrayAccess =
          access.object.Type() == ASTNodeType::ARRAY_ACCESS;
      if (!srcFromArrayAccess &&
          (mask(srcType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
        return srcReg;
      }
      u16 dest = AllocateRegister();
      builder.EmitInstruction(OP_VEC_EXTRACT, dest, srcReg, componentIndex);
      // Get the scalar type from the vector type
      CoreType vectorType = GetRegisterType(srcReg);
      CoreType scalarType = GetScalarComponentType(vectorType);
      SetRegisterType(dest, scalarType);
      return dest;
    }

    // Multi-component swizzle (.xyz/.rgb/.wzy/etc): parse each character
    // from the member name. Supports any 2-, 3-, or 4-character combination
    // of x/y/z/w or r/g/b/a.
    {
      u8 indices[4] = {0, 0, 0, 0};
      u32 swizzleLen = 0;
      if (DecodeSwizzleByHash(access.member.nameHash, access.member.nameLength,
                              indices, &swizzleLen)) {
        u16 srcReg = objectReg;
        if (objectReg < MAX_REGISTERS &&
            (program.registerStorageInfo[objectReg] &
             IR::IRProgram::STORAGE_IS_PTR)) {
          srcReg = AllocateRegister();
          CoreType loadType = GetRegisterType(objectReg);
          SetRegisterType(srcReg, loadType);
          builder.EmitInstruction(OP_STORAGE_LOAD, srcReg, objectReg);
        }
        u16 dest = AllocateRegister();
        u32 shuffleMask = 0;
        for (u32 i = 0; i < swizzleLen; i++) {
          shuffleMask |= (indices[i] & 0xF) << (i * 4);
        }
        builder.EmitInstruction(OP_VEC_SHUFFLE, dest, srcReg, srcReg);
        program.metadata[builder.currentInstruction - 1] = shuffleMask;

        CoreType vectorType = GetRegisterType(srcReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, GetVectorType(scalarType, swizzleLen));
        return dest;
      }
    }

    // Not a simple component access, fall through to return object
    return objectReg;
  }

  // Handle any expression object (function call, binary/unary expression,
  // constructor, etc.) e.g., sample(tex, uv).rgb, (matrix * vec4).xyz,
  // float4(x,y,z,w).xy
  if (access.object.Type() != ASTNodeType::IDENTIFIER) {
    // Lower the expression first to get the result
    u16 objectReg = LowerExpression(access.object);

    // Struct-field access on a non-identifier expression (e.g.,
    // `makeF(1.0).v`, `(a + b).field`). Without this, falling through
    // to the swizzle-only path returns the whole object for unknown
    // member names and downstream component extracts hit an
    // out-of-bounds index on the composite.
    if (objectReg < MAX_REGISTERS) {
      u32 structTypeHash = program.registerStructTypes[objectReg];
      if (structTypeHash != 0) {
        auto structIt = structTypeMap.find(structTypeHash);
        if (structIt != structTypeMap.end()) {
          u32 structIdx = structIt->second;
          const IRProgram::StructTypeInfo &info =
              program.structTypes[structIdx];
          for (u32 i = 0; i < info.fieldCount; i++) {
            if (program.structFieldNameHashes[info.fieldOffset + i] ==
                access.member.nameHash) {
              u16 dest = AllocateRegister();
              builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objectReg, i);
              program.metadata[builder.currentInstruction - 1] =
                  structTypeHash;
              CoreType fieldType = static_cast<CoreType>(
                  program.structFieldTypes[info.fieldOffset + i]);
              SetRegisterType(dest, fieldType);
              // Propagate nested-struct type info for further chaining.
              if ((fieldType == CoreType::CUSTOM ||
                   fieldType == CoreType::ENUM) &&
                  program.structFieldTypeHashes && dest < MAX_REGISTERS) {
                u32 fieldTypeHash =
                    program.structFieldTypeHashes[info.fieldOffset + i];
                if (fieldTypeHash != 0) {
                  program.registerStructTypes[dest] = fieldTypeHash;
                }
              }
              return dest;
            }
          }
        }
      }
    }

    // Then apply swizzle/member access
    u32 memberHash = access.member.nameHash;
    u32 componentIndex = 0xFFFFFFFF;

    // Check for single component swizzle (x/y/z/w or r/g/b/a)
    if (memberHash == Utils::HashStr("x") ||
        memberHash == Utils::HashStr("r"))
      componentIndex = 0;
    else if (memberHash == Utils::HashStr("y") ||
             memberHash == Utils::HashStr("g"))
      componentIndex = 1;
    else if (memberHash == Utils::HashStr("z") ||
             memberHash == Utils::HashStr("b"))
      componentIndex = 2;
    else if (memberHash == Utils::HashStr("w") ||
             memberHash == Utils::HashStr("a"))
      componentIndex = 3;

    if (componentIndex != 0xFFFFFFFF) {
      CoreType objType = GetRegisterType(objectReg);
      if ((mask(objType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
        return objectReg;
      }
      u16 dest = AllocateRegister();
      builder.EmitInstruction(OP_VEC_EXTRACT, dest, objectReg,
                              componentIndex);
      // Get the scalar type from the vector type
      CoreType vectorType = GetRegisterType(objectReg);
      CoreType scalarType = GetScalarComponentType(vectorType);
      SetRegisterType(dest, scalarType);
      return dest;
    }

    // Check for multi-component swizzle (rgb, xyz, etc.)
    static const u32 HASH_RGB = Utils::HashStr("rgb");
    static const u32 HASH_XYZ = Utils::HashStr("xyz");
    static const u32 HASH_XY = Utils::HashStr("xy");
    static const u32 HASH_RG = Utils::HashStr("rg");

    if (memberHash == HASH_RGB || memberHash == HASH_XYZ) {
      // Extract first 3 components (vec4 -> vec3)
      u16 dest = AllocateRegister();
      // For VEC_SHUFFLE: operand0 = src0, operand1 = src1 (same for
      // single-source swizzle) Metadata = shuffle mask (4 bits per component)
      u32 shuffleMask = (0 << 0) | (1 << 4) | (2 << 8); // components 0, 1, 2
      builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objectReg, objectReg);
      program.metadata[builder.currentInstruction - 1] = shuffleMask;
      // Get the scalar type and build the appropriate vec3 type
      CoreType vectorType = GetRegisterType(objectReg);
      CoreType scalarType = GetScalarComponentType(vectorType);
      SetRegisterType(dest, GetVectorType(scalarType, 3));
      return dest;
    }

    if (memberHash == HASH_XY || memberHash == HASH_RG) {
      // Extract first 2 components (vec4/vec3 -> vec2)
      u16 dest = AllocateRegister();
      u32 shuffleMask = (0 << 0) | (1 << 4); // components 0, 1
      builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objectReg, objectReg);
      program.metadata[builder.currentInstruction - 1] = shuffleMask;
      // Get the scalar type and build the appropriate vec2 type
      CoreType vectorType = GetRegisterType(objectReg);
      CoreType scalarType = GetScalarComponentType(vectorType);
      SetRegisterType(dest, GetVectorType(scalarType, 2));
      return dest;
    }

    // No swizzle or unknown member, return object as-is
    return objectReg;
  }

  const IdentifierData &obj = ast->GetIdentifier(access.object);

  switch (obj.identifierKind) {
  case SpecialIdentifier::ATTRIBUTES: {
    u16 dest = AllocateRegister();
    u32 attrIndex = GetAttributeIndex(access.member.nameHash);
    builder.EmitInstruction(OP_LOAD_ATTR, dest, attrIndex);

    // Check if attribute is compressed - if so, return raw uint type
    CompressionFormat compression = GetAttributeCompression(attrIndex);
    CoreType attrType = CoreType::FLOAT3; // Default for position/normal

    if (compression != CompressionFormat::NONE) {
      // Compressed attributes load raw uint data
      attrType = GetRawTypeForCompression(compression);
    } else {
      // Look up semantic type from AST pipeline attributes
      attrType = GetInputTypeFromAttribute(access.member.nameHash);
    }
    SetRegisterType(dest, attrType);
    return dest;
  }

  case SpecialIdentifier::OUTPUT: {
    u16 dest = AllocateRegister();
    u32 nameHash = access.member.nameHash;
    u32 slot = ResolveOutputSlotForLoad(nameHash);
    CoreType outputType = ResolveOutputTypeForLoad(nameHash);
    builder.EmitInstruction(OP_LOAD_OUTPUT, dest, slot);
    program.metadata[builder.currentInstruction - 1] = nameHash;
    SetRegisterType(dest, outputType);
    return dest;
  }

  case SpecialIdentifier::RESOURCES: {
    // Resources are registered with their short name in RESOURCES namespace
    // The namespace disambiguates them from other symbols
    Symbol *sym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), access.member.nameHash);
    if (!sym || sym->kind != SymbolKind::RESOURCE)
      return 0;

    const ResourceData &resData = symbols->resources[sym->index];
    u16 dest = AllocateRegister();

    switch (resData.type) {
    case ResourceBinding::UniformBuffer:
      // Uniform buffers (declared with "uniform" keyword) contain a single
      // value They emit OP_LOAD_UNIFORM to load the value directly
      builder.EmitInstruction(OP_LOAD_UNIFORM, dest, resData.bindingIndex);
      break;

    case ResourceBinding::StorageBuffer:
      // Storage buffers (declared with "buffer" keyword) are arrays
      // They emit OP_STORAGE_PTR to enable dynamic indexing via OpAccessChain
      builder.EmitInstruction(OP_STORAGE_PTR, dest, resData.bindingIndex);
      program.metadata[builder.currentInstruction - 1] =
          resData.structTypeHash;

      // Mark this register as holding a storage buffer pointer
      if (dest < MAX_REGISTERS) {
        program.registerStorageInfo[dest] =
            (resData.bindingIndex << IR::IRProgram::STORAGE_BINDING_SHIFT) |
            IR::IRProgram::STORAGE_IS_PTR;
      }

      // Register buffer element types for SPIR-V backend
      if (resData.bindingIndex < 32) {
        if (resData.structTypeHash != 0) {
          program.bufferElementStructTypes[resData.bindingIndex] =
              resData.structTypeHash;
        } else {
          CoreType coreType = static_cast<CoreType>(resData.coreType);
          if (coreType != CoreType::VOID && coreType != CoreType::INVALID) {
            program.bufferElementCoreTypes[resData.bindingIndex] =
                resData.coreType;
          }
        }
      }
      break;

    case ResourceBinding::Texture:
      // Traditional bound texture - encode slot in register
      dest = 0x2000 | resData.bindingIndex;
      break;

    case ResourceBinding::StorageImage:
      // Storage image (read/write texture) - encode slot same as texture
      dest = 0x2000 | resData.bindingIndex;
      break;

    case ResourceBinding::Sampler:
      dest = 0x3000 | resData.bindingIndex;
      break;

    default:
      break;
    }

    // Use explicit type from render config
    CoreType resourceType = static_cast<CoreType>(resData.coreType);
    SetRegisterType(dest, resourceType);

    // Store struct type hash for storage buffers with struct types
    // Also ensure the struct type is registered in the IRProgram
    if (resData.structTypeHash != 0 && dest < MAX_REGISTERS) {
      // Ensure struct type is registered so field info is available
      LookupOrRegisterStructType(resData.structTypeHash);
      program.registerStructTypes[dest] = resData.structTypeHash;
    }

    return dest;
  }

  case SpecialIdentifier::INPUT: {
    u16 dest = AllocateRegister();
    u32 memberHash = access.member.nameHash;

    // Check for built-in inputs first
    switch (memberHash) {
    case BuiltinHash::VERTEX_ID:
      if (currentStage != ShaderStage::Vertex) {
        ReportError(
            "Error: input.vertex_id is only available in vertex shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::VERTEX_ID);
      SetRegisterType(dest, CoreType::UINT);
      return dest;

    case BuiltinHash::INSTANCE_ID:
      if (currentStage != ShaderStage::Vertex) {
        ReportError(
            "Error: input.instance_id is only available in vertex shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::INSTANCE_ID);
      SetRegisterType(dest, CoreType::UINT);
      return dest;

    case BuiltinHash::GLOBAL_ID:
      if (currentStage != ShaderStage::Compute) {
        ReportError(
            "Error: input.global_id is only available in compute shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::GLOBAL_INVOCATION_ID);
      SetRegisterType(dest, CoreType::UINT3);
      return dest;

    case BuiltinHash::LOCAL_ID:
      if (currentStage != ShaderStage::Compute) {
        ReportError(
            "Error: input.local_id is only available in compute shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::LOCAL_INVOCATION_ID);
      SetRegisterType(dest, CoreType::UINT3);
      return dest;

    case BuiltinHash::WORKGROUP_ID:
      if (currentStage != ShaderStage::Compute) {
        ReportError("Error: input.workgroup_id is only available in "
                    "compute shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::WORKGROUP_ID);
      SetRegisterType(dest, CoreType::UINT3);
      return dest;

    case BuiltinHash::NUM_WORKGROUPS:
      if (currentStage != ShaderStage::Compute) {
        ReportError("Error: input.num_workgroups is only available in "
                    "compute shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::NUM_WORKGROUPS);
      SetRegisterType(dest, CoreType::UINT3);
      return dest;

    case BuiltinHash::LOCAL_INDEX:
      if (currentStage != ShaderStage::Compute) {
        ReportError("Error: input.local_index is only available in "
                    "compute shaders\n");
        return 0;
      }
      builder.EmitInstruction(OP_LOAD_INPUT, dest,
                              BuiltinInputSlot::LOCAL_INVOCATION_INDEX);
      SetRegisterType(dest, CoreType::UINT);
      return dest;

    case BuiltinHash::POSITION:
      // In fragment shaders, input.position refers to gl_FragCoord
      if (currentStage == ShaderStage::Fragment) {
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::FRAG_COORD);
        SetRegisterType(dest, CoreType::FLOAT4);
        return dest;
      }
      // In other stages, fall through to varying lookup
      break;

    default:
      break;
    }

    // Fragment shader reading interpolated varyings from vertex output
    // input.xxx -> OP_LOAD_INPUT with slot index

    // Map varying name to slot index using pass context if available
    u32 inputSlot = GetInputSlotIndex(memberHash);
    builder.EmitInstruction(OP_LOAD_INPUT, dest, inputSlot);
    if (inputSlot < 32 && currentPassVaryings) {
      u32 varyingIndex = inputSlot - OutputSlot::VARYING0;
      program.inputInterpolations[inputSlot] =
          static_cast<u8>(currentPassVaryings->GetInterpolation(varyingIndex));
    }

    // Get type from varying context if available (preferred)
    // Otherwise fall back to pipeline attribute declarations
    CoreType inputType;
    if (currentPassVaryings) {
      // inputSlot is VARYING0 + index, so subtract VARYING0 to get raw index
      u32 varyingIndex = inputSlot - OutputSlot::VARYING0;
      inputType = currentPassVaryings->GetType(varyingIndex);
    } else {
      inputType = GetInputTypeFromAttribute(memberHash);
    }
    SetRegisterType(dest, inputType);
    return dest;
  }

  case SpecialIdentifier::SELF:
  case SpecialIdentifier::NONE: {
    // Regular struct member access
    u16 objReg = LowerExpression(access.object);

    // Look up the variable's struct type
    u32 structTypeHash = 0;
    auto varIt = variableStructTypes.find(obj.name.nameHash);
    if (varIt != variableStructTypes.end()) {
      structTypeHash = varIt->second;
    } else if (objReg < MAX_REGISTERS) {
      structTypeHash = program.registerStructTypes[objReg];
    }

    if (structTypeHash != 0) {
      // Find the field index and type
      u32 fieldIndex = 0xFFFFFFFF;
      CoreType fieldType = CoreType::FLOAT;
      u32 fieldOffset = 0;
      auto structIt = structTypeMap.find(structTypeHash);
      if (structIt != structTypeMap.end()) {
        u32 structIdx = structIt->second;
        const IRProgram::StructTypeInfo &info =
            program.structTypes[structIdx];
        fieldOffset = info.fieldOffset;
        for (u32 i = 0; i < info.fieldCount; i++) {
          if (program.structFieldNameHashes[info.fieldOffset + i] ==
              access.member.nameHash) {
            fieldIndex = i;
            fieldType = static_cast<CoreType>(
                program.structFieldTypes[info.fieldOffset + i]);
            break;
          }
        }
      }

      if (fieldIndex != 0xFFFFFFFF) {
        u16 dest = AllocateRegister();
        // Emit struct extract: dest = struct.field[fieldIndex]
        builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objReg, fieldIndex);
        // Store full struct type hash in metadata (field index is in operand
        // 1)
        program.metadata[builder.currentInstruction - 1] = structTypeHash;
        SetRegisterType(dest, fieldType);
        if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
            program.structFieldTypeHashes) {
          u32 fieldTypeHash =
              program.structFieldTypeHashes[fieldOffset + fieldIndex];
          if (fieldTypeHash != 0 && dest < MAX_REGISTERS) {
            program.registerStructTypes[dest] = fieldTypeHash;
          }
        }
        return dest;
      }
    }

    // Fallback: try vector component access (e.g., pos.x, pos.y, pos.z)
    u32 componentIndex = 0xFFFFFFFF;
    if (access.member.nameHash == Utils::HashStr("x") ||
        access.member.nameHash == Utils::HashStr("r"))
      componentIndex = 0;
    else if (access.member.nameHash == Utils::HashStr("y") ||
             access.member.nameHash == Utils::HashStr("g"))
      componentIndex = 1;
    else if (access.member.nameHash == Utils::HashStr("z") ||
             access.member.nameHash == Utils::HashStr("b"))
      componentIndex = 2;
    else if (access.member.nameHash == Utils::HashStr("w") ||
             access.member.nameHash == Utils::HashStr("a"))
      componentIndex = 3;

    if (componentIndex != 0xFFFFFFFF) {
      CoreType objType = GetRegisterType(objReg);
      if ((mask(objType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
        return objReg;
      }
      u16 dest = AllocateRegister();
      builder.EmitInstruction(OP_VEC_EXTRACT, dest, objReg, componentIndex);
      // Get the scalar type from the vector type
      CoreType vectorType = GetRegisterType(objReg);
      CoreType scalarType = GetScalarComponentType(vectorType);
      SetRegisterType(dest, scalarType);
      return dest;
    }

    // Multi-component swizzle reads: parse each character of the member
    // name. Accepts any 2/3/4-character combination of x/y/z/w or r/g/b/a
    // (mixing the two sets is rejected).
    {
      u8 indices[4] = {0, 0, 0, 0};
      u32 swizzleLen = 0;
      if (DecodeSwizzleByHash(access.member.nameHash, access.member.nameLength,
                              indices, &swizzleLen)) {
        u16 dest = AllocateRegister();
        u32 shuffleMask = 0;
        for (u32 i = 0; i < swizzleLen; i++) {
          shuffleMask |= (indices[i] & 0xF) << (i * 4);
        }
        builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objReg, objReg);
        program.metadata[builder.currentInstruction - 1] = shuffleMask;

        CoreType vectorType = GetRegisterType(objReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        CoreType resultType = GetVectorType(scalarType, swizzleLen);
        SetRegisterType(dest, resultType);
        return dest;
      }
    }

    // Unknown member access - emit placeholder
    u16 dest = AllocateRegister();
    builder.EmitInstruction(OP_LOAD_REG, dest, objReg);
    program.metadata[builder.currentInstruction - 1] = access.member.nameHash;
    // Copy type from source register
    CoreType srcType = GetRegisterType(objReg);
    SetRegisterType(dest, srcType);
    return dest;
  }

  default:
    return 0;
  }
}

inline bool IRLowering::IsBuiltinOutput(u32 nameHash) {
  static const u32 HASH_POSITION = Utils::HashStr("position");
  static const u32 HASH_DEPTH = Utils::HashStr("depth");
  if (nameHash == HASH_POSITION)
    return currentStage == ShaderStage::Vertex;
  if (currentStage == ShaderStage::Fragment) {
    return nameHash == HASH_DEPTH || IsAllowedFragmentOutput(nameHash);
  }
  return false;
}

inline u32 IRLowering::GetBuiltinOutputSlot(u32 nameHash) {
  static const u32 HASH_POSITION = Utils::HashStr("position");
  static const u32 HASH_COLOR = Utils::HashStr("color");
  static const u32 HASH_DEPTH = Utils::HashStr("depth");
  if (nameHash == HASH_POSITION)
    return OutputSlot::POSITION;
  if (nameHash == HASH_DEPTH)
    return OutputSlot::DEPTH;
  if (currentStage == ShaderStage::Fragment) {
    const FragmentOutputDeclData *decl = FindFragmentOutput(nameHash);
    if (decl) {
      return OutputSlot::FragmentColor(decl->location);
    }
    if (nameHash == HASH_COLOR && (!currentPassData || !currentPassData->hasFragmentOutputs)) {
      return OutputSlot::COLOR;
    }
  }
  return OutputSlot::VARYING0; // Fallback
}

inline CoreType IRLowering::GetBuiltinOutputType(u32 nameHash) {
  static const u32 HASH_POSITION = Utils::HashStr("position");
  static const u32 HASH_COLOR = Utils::HashStr("color");
  static const u32 HASH_DEPTH = Utils::HashStr("depth");
  if (nameHash == HASH_POSITION)
    return CoreType::FLOAT4;
  if (nameHash == HASH_DEPTH)
    return CoreType::FLOAT;
  if (currentStage == ShaderStage::Fragment) {
    CoreType fragmentType = GetFragmentOutputType(nameHash);
    if (fragmentType != CoreType::INVALID) {
      return fragmentType;
    }
    if (nameHash == HASH_COLOR && (!currentPassData || !currentPassData->hasFragmentOutputs)) {
      return CoreType::INVALID;
    }
  }
  return CoreType::INVALID;
}

inline const FragmentOutputDeclData *
IRLowering::FindFragmentOutput(u32 nameHash) const {
  if (!currentPassData || !currentPassData->hasFragmentOutputs) {
    return nullptr;
  }
  for (u32 i = 0; i < currentPassData->fragmentOutputs.count; i++) {
    if (currentPassData->fragmentOutputs[i].name.nameHash == nameHash) {
      return &currentPassData->fragmentOutputs[i];
    }
  }
  return nullptr;
}

inline bool IRLowering::IsAllowedFragmentOutput(u32 nameHash) const {
  static const u32 HASH_COLOR = Utils::HashStr("color");
  static const u32 HASH_DEPTH = Utils::HashStr("depth");
  if (nameHash == HASH_DEPTH) {
    return true;
  }
  if (!currentPassData || !currentPassData->hasFragmentOutputs) {
    return nameHash == HASH_COLOR;
  }
  return FindFragmentOutput(nameHash) != nullptr;
}

inline CoreType IRLowering::GetFragmentOutputType(u32 nameHash) const {
  const FragmentOutputDeclData *decl = FindFragmentOutput(nameHash);
  if (!decl) {
    return CoreType::INVALID;
  }
  return decl->typeInfo.coreType;
}

static inline bool IsPerspectiveInterpolationType(CoreType type) {
  return type == CoreType::FLOAT || type == CoreType::FLOAT2 ||
         type == CoreType::FLOAT3 || type == CoreType::FLOAT4;
}

inline u32 IRLowering::ResolveOutputSlotForStore(
    u32 nameHash, CoreType valueType, const char *nameStr,
    InterpolationMode interpolation) {
  if (currentStage == ShaderStage::Fragment && !IsBuiltinOutput(nameHash)) {
    char msg[256];
    if (nameStr && nameStr[0] != '\0') {
      snprintf(msg, sizeof(msg),
               "Error: fragment output '%s' is not declared in this pass's outputs block\n",
               nameStr);
    } else {
      snprintf(msg, sizeof(msg),
               "Error: fragment output is not declared in this pass's outputs block\n");
    }
    ReportError(msg);
    return OutputSlot::COLOR;
  }
  if (IsBuiltinOutput(nameHash)) {
    if (interpolation != InterpolationMode::Default) {
      ReportError("Error: interpolation decorators can only be used on vertex-to-fragment varyings\n");
    }
    return GetBuiltinOutputSlot(nameHash);
  }
  if (interpolation == InterpolationMode::NoPerspective &&
      !IsPerspectiveInterpolationType(valueType)) {
    ReportError("Error: @noperspective can only be used on floating-point varyings\n");
  }
  if (interpolation != InterpolationMode::Default &&
      (currentStage != ShaderStage::Vertex || !currentPassVaryings)) {
    ReportError("Error: interpolation decorators can only be used on vertex-to-fragment varyings\n");
  }
  if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
    bool conflict = false;
    u32 varyingIndex = currentPassVaryings->AddOrGetSlot(
        nameHash, valueType, nameStr, interpolation, &conflict);
    if (conflict) {
      ReportError("Error: conflicting interpolation decorators for varying\n");
    }
    u32 slot = OutputSlot::VARYING0 + varyingIndex;
    if (slot < 32) {
      program.outputInterpolations[slot] = static_cast<u8>(
          currentPassVaryings->GetInterpolation(varyingIndex));
    }
    return slot;
  }
  return OutputSlot::VARYING0;
}

inline u32 IRLowering::ResolveOutputSlotForLoad(u32 nameHash) {
  if (currentStage == ShaderStage::Fragment && !IsBuiltinOutput(nameHash)) {
    ReportError("Error: fragment output load refers to an undeclared output\n");
    return OutputSlot::COLOR;
  }
  if (IsBuiltinOutput(nameHash)) {
    return GetBuiltinOutputSlot(nameHash);
  }
  if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
    s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
    if (varyingIndex >= 0) {
      return OutputSlot::VARYING0 + (u32)varyingIndex;
    }
  }
  return OutputSlot::VARYING0;
}

inline CoreType IRLowering::ResolveOutputTypeForLoad(u32 nameHash) {
  CoreType builtinType = GetBuiltinOutputType(nameHash);
  if (builtinType != CoreType::INVALID) {
    return builtinType;
  }
  if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
    s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
    if (varyingIndex >= 0) {
      return currentPassVaryings->GetType((u32)varyingIndex);
    }
  }
  return CoreType::FLOAT4;
}

inline u32 IRLowering::GetAttributeIndex(u32 nameHash) {
  // Look up attribute by name in current pipeline's attribute list
  // Returns the declaration-order index (position is always 0)
  if (currentPipeline.IsNull())
    return 0;

  const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
  for (u32 i = 0; i < pipeline.attributes.count; i++) {
    NodeRef attrRef = pipeline.attributes[i];
    if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
      const AttributeDeclData &attr = ast->GetAttributeDecl(attrRef);
      if (attr.name.nameHash == nameHash) {
        return attr.attributeIndex;
      }
    }
  }
  return 0; // Fallback to position
}

inline u32 IRLowering::GetInputSlotIndex(u32 nameHash) {
  // Fragment shader input slot indices (matching vertex output locations)
  // For varyings passed from vertex to fragment shader
  //
  // IMPORTANT: Slots must match vertex output slots which use VARYING0 +
  // index so fragment inputs must also use VARYING0 + index for locations to
  // match

  // First, check the pass varying context for dynamic lookup
  // This allows any name used in output.xxx to be used in input.xxx
  if (currentPassVaryings) {
    s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
    if (varyingIndex >= 0) {
      // Return VARYING0 + index to match vertex output slot assignments
      return OutputSlot::VARYING0 + (u32)varyingIndex;
    }
    // Not found in vertex outputs - fall through to legacy mappings
  }

  // Fallback to legacy hardcoded mappings for backward compatibility
  // These also need to use VARYING0 + offset to match vertex outputs
  static const u32 NORMAL_HASH = Utils::HashStr("normal");
  static const u32 TEXCOORD_HASH = Utils::HashStr("texcoord");
  static const u32 COLOR_HASH = Utils::HashStr("color");
  static const u32 TANGENT_HASH = Utils::HashStr("tangent");
  static const u32 WORLDPOS_HASH = Utils::HashStr("worldPosition");

  // Slot indices use VARYING0 + offset to match vertex output slots
  if (nameHash == NORMAL_HASH)
    return OutputSlot::VARYING0 + 0;
  if (nameHash == TEXCOORD_HASH)
    return OutputSlot::VARYING0 + 1;
  if (nameHash == COLOR_HASH)
    return OutputSlot::VARYING0 + 2;
  if (nameHash == TANGENT_HASH)
    return OutputSlot::VARYING0 + 3;
  if (nameHash == WORLDPOS_HASH)
    return OutputSlot::VARYING0 + 4;
  return OutputSlot::VARYING0;
}

inline CoreType IRLowering::GetInputTypeFromAttribute(u32 nameHash) {
  if (currentPipeline.IsNull())
    return CoreType::FLOAT3;

  // Type name hashes for common types
  static const u32 FLOAT_HASH = Utils::HashStr("float");
  static const u32 FLOAT2_HASH = Utils::HashStr("float2");
  static const u32 FLOAT3_HASH = Utils::HashStr("float3");
  static const u32 FLOAT4_HASH = Utils::HashStr("float4");
  static const u32 INT_HASH = Utils::HashStr("int");
  static const u32 INT2_HASH = Utils::HashStr("int2");
  static const u32 INT3_HASH = Utils::HashStr("int3");
  static const u32 INT4_HASH = Utils::HashStr("int4");
  static const u32 UINT_HASH = Utils::HashStr("uint");
  static const u32 UINT2_HASH = Utils::HashStr("uint2");
  static const u32 UINT3_HASH = Utils::HashStr("uint3");
  static const u32 UINT4_HASH = Utils::HashStr("uint4");

  const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
  for (u32 i = 0; i < pipeline.attributes.count; i++) {
    NodeRef attrRef = pipeline.attributes[i];
    if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
      const AttributeDeclData &attrDecl = ast->GetAttributeDecl(attrRef);
      if (attrDecl.name.nameHash == nameHash) {
        // Found the attribute, parse its type
        u32 typeHash = attrDecl.dataType.nameHash;
        if (typeHash == FLOAT_HASH)
          return CoreType::FLOAT;
        if (typeHash == FLOAT2_HASH)
          return CoreType::FLOAT2;
        if (typeHash == FLOAT3_HASH)
          return CoreType::FLOAT3;
        if (typeHash == FLOAT4_HASH)
          return CoreType::FLOAT4;
        if (typeHash == INT_HASH)
          return CoreType::INT;
        if (typeHash == INT2_HASH)
          return CoreType::INT2;
        if (typeHash == INT3_HASH)
          return CoreType::INT3;
        if (typeHash == INT4_HASH)
          return CoreType::INT4;
        if (typeHash == UINT_HASH)
          return CoreType::UINT;
        if (typeHash == UINT2_HASH)
          return CoreType::UINT2;
        if (typeHash == UINT3_HASH)
          return CoreType::UINT3;
        if (typeHash == UINT4_HASH)
          return CoreType::UINT4;
      }
    }
  }
  return CoreType::FLOAT3; // Default fallback
}

inline CompressionFormat IRLowering::GetAttributeCompression(u32 attrIndex) {
  if (currentPipeline.IsNull())
    return CompressionFormat::NONE;

  const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
  for (u32 i = 0; i < pipeline.attributes.count; i++) {
    NodeRef attrRef = pipeline.attributes[i];
    if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
      const AttributeDeclData &attrDecl = ast->GetAttributeDecl(attrRef);
      if (attrDecl.attributeIndex == attrIndex) {
        // Parse compression format from the ArenaString hash
        return ParseCompressionFormat(attrDecl.compression.nameHash);
      }
    }
  }
  return CompressionFormat::NONE;
}


} // namespace BWSL::IR
