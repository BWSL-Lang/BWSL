// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.
#pragma once
#include "bwsl_ir_lowering.h"
namespace BWSL::IR {

inline void IRLowering::RegisterLocalArray(u16 reg, CoreType elementType,
                                            u32 structHash, u32 length, u32 nameHash) {
  if (program.localArrayCount == program.localArrayCapacity) {
    u32 old = program.localArrayCapacity;
    u32 capacity = old ? old * 2 : 16;
    auto grow = [&](auto*& data) {
      using Element = std::remove_pointer_t<std::remove_reference_t<decltype(data)>>;
      Element* next = static_cast<Element*>(pool->Allocate(capacity * sizeof(Element), 64));
      if (old) memcpy(next, data, old * sizeof(Element));
      data = next;
    };
    grow(program.localArrayNameHashes);
    grow(program.localArrayTypes);
    grow(program.localArrayStructTypes);
    grow(program.localArraySizes);
    grow(program.localArrayRegisters);
    program.localArrayCapacity = capacity;
  }
  u32 index = program.localArrayCount++;
  program.localArrayNameHashes[index] = nameHash;
  program.localArrayTypes[index] = static_cast<u16>(elementType);
  program.localArrayStructTypes[index] = structHash;
  program.localArraySizes[index] = length;
  program.localArrayRegisters[index] = reg;
  program.registerStorageInfo[reg] = (index << IRProgram::STORAGE_BINDING_SHIFT) |
      IRProgram::STORAGE_IS_PTR | IRProgram::STORAGE_IS_LOCAL_ARRAY;
  SetRegisterType(reg, elementType);
  program.registerStructTypes[reg] = structHash;
}

// A `T[2][3]` parameter binds a flattened copy of the argument; record the
// parameter's declared shape on that copy so `.length` sees the dimensions.
inline void IRLowering::RecordParameterArrayDimensions(
    u16 paramReg, const FunctionDeclData &fn, u32 paramIndex) {
  if (paramReg >= MAX_REGISTERS || paramIndex >= fn.parameterArraySizes.count ||
      fn.parameterArraySizes[paramIndex].count < 2) {
    return;
  }
  u32 storage = program.registerStorageInfo[paramReg];
  if (storage & IRProgram::STORAGE_IS_LOCAL_ARRAY) {
    localArrayDimensions[storage >> IRProgram::STORAGE_BINDING_SHIFT] =
        fn.parameterArraySizes[paramIndex];
  }
}

inline u16 IRLowering::CopyArgumentValue(u16 source) {
  u16 copy = AllocateRegister();
  CoreType type = GetRegisterType(source);
  SetRegisterType(copy, type);
  u32 storage = source < MAX_REGISTERS ? program.registerStorageInfo[source] : 0;
  u32 structHash = source < MAX_REGISTERS ? program.registerStructTypes[source] : 0;
  program.registerStructTypes[copy] = structHash;
  auto arrayField = structArrayValueRegs.find(source);
  bool isArrayField = arrayField != structArrayValueRegs.end();
  if ((storage & IRProgram::STORAGE_IS_LOCAL_ARRAY) || isArrayField) {
    u32 array = storage >> IRProgram::STORAGE_BINDING_SHIFT;
    u32 count = isArrayField ? arrayField->second.length : program.localArraySizes[array];
    type = isArrayField ? arrayField->second.elemType : static_cast<CoreType>(program.localArrayTypes[array]);
    structHash = isArrayField ? arrayField->second.elemStructHash : program.localArrayStructTypes[array];
    if (structHash) {
      u32 canonical = LookupOrRegisterStructType(structHash);
      if (canonical) structHash = canonical;
    }
    RegisterLocalArray(copy, type, structHash, count, 0);
    // One bounded copy loop works for arbitrary fixed lengths without emitting
    // one IR register per element. Struct elements copy their complete value.
    u16 index = AllocateRegister();
    SetRegisterType(index, CoreType::UINT);
    builder.EmitInstruction(OP_STORE_REG, index, EmitConstantUint(0));
    u32 header = builder.currentInstruction;
    u16 condition = AllocateRegister();
    SetRegisterType(condition, CoreType::BOOL);
    builder.EmitInstruction(OP_ULT, condition, index, EmitConstantUint(count));
    u32 branch = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, condition);
    u32 body = builder.currentInstruction;
    u16 value = AllocateRegister();
    SetRegisterType(value, type);
    program.registerStructTypes[value] = structHash;
    if (isArrayField) {
      const StructArrayValueInfo& field = arrayField->second;
      builder.EmitInstruction(OP_STRUCT_ARRAY_EXTRACT, value, field.parentStructReg, field.fieldIndex, index);
      program.metadata[builder.currentInstruction - 1] = field.parentStructHash;
    } else {
      builder.EmitInstruction(OP_ARRAY_LOAD, value, source, index);
    }
    builder.EmitInstruction(OP_ARRAY_STORE, copy, index, value);
    u32 continuation = builder.currentInstruction;
    builder.EmitInstruction(OP_IADD, index, index, EmitConstantUint(1));
    u32 jump = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    program.metadata[jump] = header;
    u32 merge = builder.currentInstruction;
    builder.EmitInstruction(OP_NOP, 0, 0);
    program.SetBranchTargets(branch, body, merge);
    program.structureInfo[branch] = IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, merge);
    program.continueInfo[branch] = continuation;
  } else {
    // Pointer parameters copy the pointer value, preserving its pointee. Plain
    // values do not inherit the caller's address-taken or storage metadata.
    if (storage & IRProgram::STORAGE_IS_PTR) {
      program.registerStorageInfo[copy] = storage;
      DeclarePointerVariable(copy, source);
    }
    builder.EmitInstruction(OP_STORE_REG, copy, source);
  }
  return copy;
}

// ============= Pointer arguments =============

inline OverloadTypeMask IRLowering::ArgumentOverloadMask(NodeRef argument, u16 reg) {
  bool isRegister = IR::IsRegister(reg);
  if (IsPointerArgument(argument, reg)) {
    CoreType pointee =
        static_cast<CoreType>((program.registerStorageInfo[reg] >> 8) & 0xFF);
    u32 customHash =
        pointee == CoreType::CUSTOM ? program.registerStructTypes[reg] : 0;
    return MakeOverloadMask(pointee, customHash) | OVERLOAD_POINTER_MASK;
  }
  CoreType type = GetRegisterType(reg);
  u32 customHash = type == CoreType::CUSTOM && isRegister
                       ? program.registerStructTypes[reg] : 0;
  return MakeOverloadMask(type, customHash);
}

inline void IRLowering::BindPointerParameter(const FunctionCallData &call,
                                             const FunctionDeclData &function,
                                             u32 index, u16 arg) {
  NodeRef argument = index < call.arguments.count ? call.arguments[index] : NodeRef::Null();
  if (InPointerLoop()) {
    // Checked when the loop ends, against every target it can hold.
    pointerEvents.push_back({PointerEvent::ARGUMENT, false, arg, 0,
                             static_cast<u32>(pointerEventArguments.size()), NodeRef::Null()});
    pointerEventArguments.push_back({arg, argument, function.name.nameHash, index});
  } else if (auto problem = pointerProblems.find(arg); problem != pointerProblems.end()) {
    ReportPointerArgumentProblem(argument, function.name.nameHash, index, problem->second);
  }
  // The copy keeps the pointer's target, so writes through it reach the
  // caller's variable, and two parameters given the same address alias.
  u32 nameHash = function.parameters[index].first.nameHash;
  u16 reg = CopyArgumentValue(arg);
  variableRegisters[nameHash] = reg;
  resourceHandleVariables.erase(nameHash);
  constVariables.erase(nameHash);
  initializedVariables.insert(nameHash);
  program.registerNameHashes[reg] = nameHash;
}

inline void IRLowering::ReportPointerArgumentProblem(NodeRef argument,
                                                     u32 functionNameHash,
                                                     u32 index,
                                                     PointerProblem problem) {
  if (argument.IsValid() && !reportedPointerArguments.insert(argument.packed).second)
    return;
  const char *reason = "";
  switch (problem) {
  case PointerProblem::NON_LOCAL:
    reason = "points to a resource, stage input or output, or shared "
             "memory; copy the value into a local variable, pass `^local` "
             "and store the result back";
    break;
  case PointerProblem::TEMPORARY:
    reason = "points to a temporary copy; pass the address of a local "
             "variable, one of its struct fields or an array element, not "
             "a vector component, matrix column or computed value";
    break;
  case PointerProblem::WHOLE_ARRAY:
    reason = "points to a whole array; pass a pointer to one element "
             "(`^values[i]`) or wrap the array in a struct";
    break;
  }
  char msg[384];
  snprintf(msg, sizeof(msg), "Error: pointer argument %u of '%s' %s\n",
           index + 1, ReverseLookup::GetString(functionNameHash).c_str(), reason);
  ReportErrorAt(argument, msg);
}

inline void IRLowering::ReportPointerStoreProblem(NodeRef target,
                                                  PointerProblem problem) {
  if (target.IsValid() && !reportedPointerArguments.insert(target.packed).second)
    return;
  const char *reason = "";
  switch (problem) {
  case PointerProblem::NON_LOCAL:
    reason = "a resource, stage input or output, or shared memory";
    break;
  case PointerProblem::TEMPORARY:
    reason = "a temporary copy (a vector component, matrix column or "
             "computed value)";
    break;
  case PointerProblem::WHOLE_ARRAY:
    reason = "a whole array";
    break;
  }
  char msg[256];
  snprintf(msg, sizeof(msg),
           "Error: this write through a pointer would be lost: the pointer "
           "points to %s; assign to the place directly\n", reason);
  ReportErrorAt(target, msg);
}

inline void IRLowering::ReportNonLocalPointerVariable(u16 source) {
  auto address = nonLocalAddresses.find(source);
  if (address == nonLocalAddresses.end()) return;
  ReportErrorAt(address->second,
                "Error: a pointer variable can't point to a resource, stage "
                "input or output, or shared memory; copy the value into a "
                "local variable and point to that\n");
  nonLocalAddresses.erase(address);
}

inline bool IRLowering::ReportPointerArgumentMismatch(
    const FunctionCallData &call, const FunctionDeclData &function,
    const OverloadTypeMask *argMasks, u32 argCount) {
  if (function.parameters.count != argCount) return false;
  u32 mismatch = argCount;
  for (u32 i = 0; i < argCount; i++) {
    OverloadTypeMask paramMask = MakeOverloadMaskFromResolvedTypeHash(
        function.parameters[i].second.nameHash);
    if (IsPointerParameter(function, i)) paramMask |= OVERLOAD_POINTER_MASK;
    if (OverloadMaskMatches(paramMask, argMasks[i])) continue;
    bool pointerInvolved =
        ((paramMask | argMasks[i]) & OVERLOAD_POINTER_MASK) != 0;
    // Another argument has a plain value-type mismatch, which the general
    // "Function not found" diagnostic covers.
    if (!pointerInvolved) return false;
    if (mismatch == argCount) mismatch = i;
  }
  if (mismatch == argCount) return false;

  std::string name = ReverseLookup::GetString(function.name.nameHash);
  std::string type =
      ReverseLookup::GetString(function.parameters[mismatch].second.nameHash);
  bool wantsPointer = IsPointerParameter(function, mismatch);
  bool hasPointer = (argMasks[mismatch] & OVERLOAD_POINTER_MASK) != 0;
  char msg[384];
  if (wantsPointer && !hasPointer) {
    snprintf(msg, sizeof(msg),
             "Error: argument %u of '%s' must be a pointer (%s^); pass the "
             "address of a local variable, as in `^value`\n",
             mismatch + 1, name.c_str(), type.c_str());
  } else if (!wantsPointer) {
    snprintf(msg, sizeof(msg),
             "Error: argument %u of '%s' takes a value of type %s, not a "
             "pointer; dereference it (`ptr^`)\n",
             mismatch + 1, name.c_str(), type.c_str());
  } else {
    // An argument mask names one pointee: a struct hash or a single core
    // type bit.
    OverloadTypeMask pointee = argMasks[mismatch] & ~OVERLOAD_POINTER_MASK;
    std::string passed = "another type";
    if (pointee & OVERLOAD_CUSTOM_MASK) {
      passed = ReverseLookup::GetString(static_cast<u32>(pointee));
    } else if (pointee != 0) {
      passed = ReverseLookup::GetString(GetCoreTypeNameHash(
          static_cast<CoreType>(std::countr_zero(pointee))));
    }
    snprintf(msg, sizeof(msg),
             "Error: argument %u of '%s' must point to %s, but the pointer "
             "passed points to %s\n",
             mismatch + 1, name.c_str(), type.c_str(), passed.c_str());
  }
  ReportErrorAt(mismatch < call.arguments.count ? call.arguments[mismatch]
                                                : NodeRef::Null(),
                msg);
  return true;
}

inline bool IRLowering::ReportPointerArgumentToBuiltin(
    const FunctionCallData &call, const u16 *args, u32 argCount) {
  for (u32 i = 0; i < argCount; i++) {
    NodeRef argument = i < call.arguments.count ? call.arguments[i] : NodeRef::Null();
    if (!IsPointerArgument(argument, args[i])) continue;
    std::string name = call.name.isHashOnly()
                           ? ReverseLookup::GetString(call.name.nameHash)
                           : call.name.ToString(sourceBase);
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Error: argument %u of '%s' is a pointer, but '%s' takes "
             "values; dereference it (`ptr^`)\n",
             i + 1, name.c_str(), name.c_str());
    ReportErrorAt(argument, msg);
    return true;
  }
  return false;
}

inline bool IRLowering::ReportStructMethodPointerMismatch(
    const StructData *structData, const FunctionCallData &call,
    const OverloadTypeMask *argMasks, u32 argCount) {
  for (u32 i = 0; i < structData->methodIndices.count; i++) {
    u32 index = structData->methodIndices[i];
    if (index >= symbols->functions.count) continue;
    const FunctionData &method = symbols->functions[index];
    if (method.name.nameHash != call.name.nameHash ||
        method.paramTypeMasks.count != argCount) {
      continue;
    }
    NodeRef methodRef;
    methodRef.packed = method.astNodeIndex;
    if (methodRef.Type() != ASTNodeType::FUNCTION) continue;
    return ReportPointerArgumentMismatch(call, ast->GetFunction(methodRef),
                                         argMasks, argCount);
  }
  return false;
}

inline void IRLowering::LowerEnumPatternBody(NodeRef pattern, u16 receiver,
                                             const EnumData &enumData) {
  const PatternMatchData &match = ast->GetPatternMatch(pattern);
  auto savedVariables = variableRegisters;
  auto savedStructTypes = variableStructTypes;
  auto savedConstants = constVariables;
  auto savedNodes = nodeRegisters;
  bool sumType = (enumData.flags & EnumData::IS_SUM_TYPE) != 0;
  u16 tag = receiver;
  if (sumType) {
    tag = AllocateRegister();
    SetRegisterType(tag, CoreType::INT);
    builder.EmitInstruction(OP_ENUM_TAG, tag, receiver);
  }
  u16 matched = AllocateRegister();
  SetRegisterType(matched, CoreType::BOOL);
  builder.EmitInstruction(OP_STORE_REG, matched, builder.EmitConstantBool(false));
  for (u32 a = 0; a < match.arms.count; ++a) {
    const PatternMatchData &arm = ast->GetPatternMatch(match.arms[a]);
    variableRegisters = savedVariables;
    variableStructTypes = savedStructTypes;
    constVariables = savedConstants;
    nodeRegisters = savedNodes;
    u32 variantIndex = 0xFFFFFFFF;
    for (u32 v = 0; !arm.isDefault && v < enumData.variants.count; ++v) {
      if (enumData.variants[v].name.nameHash == arm.variantHash) { variantIndex = v; break; }
    }
    u16 condition = AllocateRegister();
    SetRegisterType(condition, CoreType::BOOL);
    builder.EmitInstruction(OP_NOT, condition, matched);
    if (!arm.isDefault) {
      if (variantIndex == 0xFFFFFFFF) {
        ReportError("Error: unknown enum variant in pattern arm\n");
        return;
      }
      u32 value = sumType ? variantIndex : enumData.variants[variantIndex].value;
      u16 expected = GetRegisterType(tag) == CoreType::UINT ? EmitConstantUint(value) : EmitConstantInt(static_cast<s32>(value));
      u16 equal = AllocateRegister();
      SetRegisterType(equal, CoreType::BOOL);
      builder.EmitInstruction(OP_IEQ, equal, tag, expected);
      u16 both = AllocateRegister();
      SetRegisterType(both, CoreType::BOOL);
      builder.EmitInstruction(OP_AND, both, equal, condition);
      condition = both;
    }
    u32 branch = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, condition);
    u32 bodyStart = builder.currentInstruction;
    builder.EmitInstruction(OP_STORE_REG, matched, builder.EmitConstantBool(true));
    if (variantIndex != 0xFFFFFFFF) {
      const EnumData::Variant &variant = enumData.variants[variantIndex];
      u32 payloadIndex = 0;
      for (u32 f = 0; f < variant.associatedTypes.count; ++f) {
        CoreType type = variant.associatedTypes[f];
        u32 hash = f < variant.associatedTypeHashes.count ? variant.associatedTypeHashes[f] : 0;
        u32 width = GetEnumPayloadFieldCount(type, hash);
        if (f < arm.bindings.count && arm.bindings[f].first.nameHash != Utils::HashStr("_")) {
          u32 name = arm.bindings[f].first.nameHash;
          u32 slot = payloadIndex;
          u16 value = ReadEnumPayloadValue(receiver, &slot, type, hash);
          if (type == CoreType::CUSTOM) variableStructTypes[name] = program.registerStructTypes[value];
          variableRegisters[name] = value;
          constVariables.erase(name);
        }
        payloadIndex += width;
      }
    }
    PointerControlBoundary(controlDepth);
    controlDepth++;
    if (arm.body.Type() == ASTNodeType::BLOCK) {
      // A block ending in a bare expression yields that expression.
      const BlockData &block = ast->GetBlock(arm.body);
      NodeRef savedImplicit = implicitReturnStatement;
      implicitReturnStatement = NodeRef::Null();
      if (block.statements.count > 0) {
        NodeRef last = block.statements[block.statements.count - 1];
        switch (last.Type()) {
        case ASTNodeType::LITERAL:
        case ASTNodeType::IDENTIFIER:
        case ASTNodeType::BINARY_OP:
        case ASTNodeType::UNARY_OP:
        case ASTNodeType::MEMBER_ACCESS:
        case ASTNodeType::ARRAY_ACCESS:
        case ASTNodeType::FUNCTION_CALL:
        case ASTNodeType::TERNARY_EXPRESSION:
          implicitReturnStatement = last;
          break;
        default:
          break;
        }
      }
      LowerBlock(arm.body);
      implicitReturnStatement = savedImplicit;
    } else {
      u16 value = LowerExpression(arm.body);
      CheckStoreShape(arm.body, value, inlineReturnType, inlineReturnStructHash);
      builder.EmitInstruction(OP_STORE_REG, inlineReturnReg, value);
      SetRegisterType(inlineReturnReg, GetRegisterType(value));
      if (value < MAX_REGISTERS) program.registerStructTypes[inlineReturnReg] = program.registerStructTypes[value];
      builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg, builder.EmitConstantBool(true));
    }
    controlDepth--;
    PointerControlBoundary(controlDepth);
    u32 merge = builder.currentInstruction;
    builder.EmitInstruction(OP_NOP, 0, 0);
    program.SetBranchTargets(branch, bodyStart, merge);
    program.structureInfo[branch] = IRProgram::PackStructure(IRProgram::STRUCT_IF_HEADER, merge);
  }
  variableRegisters = savedVariables;
  variableStructTypes = savedStructTypes;
  constVariables = savedConstants;
  nodeRegisters = savedNodes;
}

inline u16 IRLowering::LowerFunctionCall(NodeRef ref) {
  const FunctionCallData &call = ast->GetFunctionCall(ref);
  // Implicit-LOD sampling and derivatives need fragment quads. The parser
  // checks stage bodies; this also covers helper functions, whose stage is
  // only known once a stage calls them. Check before a dynamic texture array
  // call expands into one call per element, so it's reported once.
  if (call.flags & FunctionCallFlags::IS_INTRINSIC) {
    const StdLib::IntrinsicData &data = StdLib::INTRINSICS[call.intrinsicIndex];
    char stageMsg[256];
    if (StdLib::FormatDerivativeStageError(
            static_cast<StdLib::Intrinsic>(data.enumIndex), currentStage,
            stageMsg, sizeof(stageMsg))) {
      std::string error = std::string("Error: ") + stageMsg + "\n";
      ReportErrorAt(ref, error.c_str());
      // Give the result the call's normal type (float4 for a sample, the
      // argument's type for a derivative) so storing or returning it doesn't
      // add a conversion error on top of this one.
      u16 dest = AllocateRegister();
      TypeMask returnTypes = data.returnTypes;
      if (std::has_single_bit(returnTypes)) {
        SetRegisterType(dest, static_cast<CoreType>(std::countr_zero(returnTypes)));
      } else if (call.arguments.count > 0) {
        SetRegisterType(dest, GetRegisterType(LowerExpression(call.arguments[0])));
      }
      return dest;
    }
  }
  u16 textureArrayResult;
  if (TryLowerDynamicTextureArrayCall(ref, &textureArrayResult))
    return textureArrayResult;

  // Collect argument registers
  // Initialize to the unused operand sentinel. Supports up to 16 arguments
  // for mat4 construction.
  u16 args[16] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
  u32 argCount = call.arguments.count < 16 ? call.arguments.count : 16;
  for (u32 i = 0; i < argCount; i++) {
    args[i] = LowerExpression(call.arguments[i]);
  }

  u16 dest = AllocateRegister();

  // Check for enum variant construction (e.g., SDFShape::Sphere(0.5))
  // The moduleObject will reference the enum type identifier
  auto resolveConstructorEnum =
      [&](NodeRef enumObject, u32 *outEnumHash) -> Symbol * {
    if (outEnumHash) {
      *outEnumHash = 0;
    }
    if (enumObject.IsNull()) {
      return nullptr;
    }

    if (enumObject.Type() == ASTNodeType::IDENTIFIER) {
      const IdentifierData &enumIdent = ast->GetIdentifier(enumObject);
      u32 enumHash = enumIdent.name.nameHash;
      if (outEnumHash) {
        *outEnumHash = enumHash;
      }
      return SymbolTable::LookupByHash(const_cast<SymbolTableData *>(symbols),
                                       enumHash);
    }

    if (enumObject.Type() == ASTNodeType::MEMBER_ACCESS) {
      const MemberAccessData &access = ast->GetMemberAccess(enumObject);
      if (access.isModuleQualified &&
          access.object.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData &moduleIdent = ast->GetIdentifier(access.object);
        u32 moduleHash = SymbolTable::ResolveModuleNameHashInScope(
            const_cast<SymbolTableData *>(symbols), moduleIdent.name.nameHash,
            AliasOwnerKind(), AliasOwnerModuleIndex());
        std::string syntheticQualifiedName;
        syntheticQualifiedName.reserve(2 + 10 + 10);
        syntheticQualifiedName.append("m")
            .append(std::to_string(moduleHash));
        syntheticQualifiedName.append("::e")
            .append(std::to_string(access.member.nameHash));
        if (outEnumHash) {
          *outEnumHash = access.member.nameHash;
        }
        return SymbolTable::LookupByHash(
            const_cast<SymbolTableData *>(symbols),
            Utils::HashStr(syntheticQualifiedName.c_str()));
      }
    }

    return nullptr;
  };

  if (!call.moduleObject.IsNull()) {
    u32 enumHash = 0;
    // Look up in symbol table to check if it's an enum type. ENUM is used for
    // module enums, ENUM_SYMBOL for pipeline-local enums.
    Symbol *enumSym = resolveConstructorEnum(call.moduleObject, &enumHash);
    if (enumSym && (enumSym->kind == SymbolKind::ENUM ||
                    enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
      const EnumData &enumData = symbols->enums[enumSym->index];

      // Find the variant by name
      u32 variantIndex = 0xFFFFFFFF;
      for (u32 v = 0; v < enumData.variants.count; v++) {
        if (enumData.variants[v].name.nameHash == call.name.nameHash) {
          variantIndex = v;
          break;
        }
      }

      if (variantIndex != 0xFFFFFFFF) {
        // Extra or missing payload arguments would be silently ignored.
        u32 expectedArgs = enumData.variants[variantIndex].associatedTypes.count;
        if (argCount != expectedArgs) {
          char msg[192];
          snprintf(msg, sizeof(msg),
                   "Error: enum variant constructor expects %u argument(s), "
                   "got %u\n",
                   expectedArgs, argCount);
          ReportError(msg);
          return 0;
        }
        // Register the enum as a struct type if not already done
        u32 enumStructHash =
            LookupOrRegisterStructType(enumData.name.nameHash);

        // A payload keeps its declared shape: a vector for a scalar field
        // would spill into the next field's storage, or be cut short. Its
        // scalar kind converts like an assignment, and slots shared with
        // another type hold its bits as floats.
        const EnumData::Variant &variant = enumData.variants[variantIndex];
        // Struct, matrix and bool vector payloads are written one scalar
        // slot at a time, and so is a payload of more than four fields,
        // which OP_ENUM_CONSTRUCT has no operands for.
        bool flatten = argCount > 4;
        for (u32 i = 0; i < argCount; i++) {
          CoreType type = variant.associatedTypes[i];
          u32 hash = i < variant.associatedTypeHashes.count
                         ? variant.associatedTypeHashes[i] : 0;
          flatten |= (mask(type) & (TypeMasks::MATRIX_TYPES | TypeMasks::BOOL_VECTORS)) != 0 ||
                     (type == CoreType::CUSTOM && hash != 0 && !IsSumEnumTypeHash(hash));
        }
        u32 slot = 0;
        for (u32 i = 0; i < argCount; i++) {
          CoreType type = variant.associatedTypes[i];
          u32 hash = i < variant.associatedTypeHashes.count
                         ? variant.associatedTypeHashes[i] : 0;
          if (!CheckStoreShape(call.arguments[i], args[i], type, hash)) {
            SetRegisterType(dest, CoreType::INVALID);
            return dest;
          }
          args[i] = ConvertRegisterToType(args[i], type);
          if (!flatten)
            args[i] = EnumPayloadToSlots(call.arguments[i], args[i], type,
                                         enumStructHash, slot);
          slot += GetEnumPayloadFieldCount(type, hash);
        }

        if (flatten) {
          std::vector<std::pair<u16, CoreType>> scalars;
          for (u32 i = 0; i < argCount; i++) {
            u32 hash = i < variant.associatedTypeHashes.count
                           ? variant.associatedTypeHashes[i] : 0;
            if (!FlattenEnumPayload(call.arguments[i], args[i],
                                    variant.associatedTypes[i], hash, scalars)) {
              SetRegisterType(dest, CoreType::INVALID);
              return dest;
            }
          }
          // The enum struct holds at most 64 payload slots
          // (RegisterEnumAsStructType).
          if (scalars.size() > 64) {
            ReportErrorAt(ref, "Error: an enum variant's payload can hold at most "
                               "64 scalars; split it across variants\n");
            SetRegisterType(dest, CoreType::INVALID);
            return dest;
          }
          u16 current = AllocateRegister();
          SetRegisterType(current, CoreType::CUSTOM);
          program.registerStructTypes[current] = enumStructHash;
          builder.EmitInstruction(OP_ENUM_CONSTRUCT, current, IR::InvalidOperand,
                                  IR::InvalidOperand, IR::InvalidOperand,
                                  IR::InvalidOperand);
          program.metadata[builder.currentInstruction - 1] =
              (variantIndex << 16) |
              ((enumHash != 0 ? enumHash : enumData.name.nameHash) & 0xFF);
          for (u32 s = 0; s < scalars.size(); s++) {
            u16 scalar = EnumPayloadToSlots(ref, scalars[s].first, scalars[s].second,
                                            enumStructHash, s);
            u16 next = AllocateRegister();
            SetRegisterType(next, CoreType::CUSTOM);
            program.registerStructTypes[next] = enumStructHash;
            builder.EmitInstruction(OP_STRUCT_INSERT, next, current,
                                    static_cast<u16>(1 + s), scalar);
            program.metadata[builder.currentInstruction - 1] = enumStructHash;
            current = next;
          }
          builder.EmitInstruction(OP_STORE_REG, dest, current);
          SetRegisterType(dest, CoreType::CUSTOM);
          program.registerStructTypes[dest] = enumStructHash;
          return dest;
        }

        // Emit OP_ENUM_CONSTRUCT
        // operands[0..3] = argument values (field data)
        builder.EmitInstruction(OP_ENUM_CONSTRUCT, dest, args[0], args[1],
                                args[2], args[3]);
        // metadata = (variantIndex << 16) | (argCount << 8) | enumStructHash
        // lower bits
        program.metadata[builder.currentInstruction - 1] =
            (variantIndex << 16) | (argCount << 8) |
            ((enumHash != 0 ? enumHash : enumData.name.nameHash) & 0xFF);

        // Set register type to the enum struct type
        SetRegisterType(dest, CoreType::CUSTOM);
        program.registerStructTypes[dest] = enumStructHash;

        return dest;
      }
    }
  }

  // Check for struct method call (e.g., basis.to_world(v)). Struct methods are
  // resolved only through the receiver type; globals with the same name are not
  // candidates for this dispatch.
  if (call.flags & FunctionCallFlags::IS_METHOD_CALL) {
    if (!call.moduleObject.IsNull()) {
      u16 receiverReg = LowerExpression(call.moduleObject);
      if (!IR::IsRegister(receiverReg)) {
        return 0;
      }

      u32 receiverStructHash = program.registerStructTypes[receiverReg];
      if (receiverStructHash != 0) {
        StructData *structData = LookupStructDataByHash(receiverStructHash);
        if (structData) {
          bool receiverIsConst = false;
          if (call.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
            const IdentifierData &receiverIdent =
                ast->GetIdentifier(call.moduleObject);
            receiverIsConst =
                constVariables.find(receiverIdent.name.nameHash) !=
                constVariables.end();
            if (receiverIdent.identifierKind == SpecialIdentifier::SELF &&
                currentStructMethodIsConst) {
              receiverIsConst = true;
            }
          }

          u16 methodResult = TryLowerStructMethodCall(
              ref, call, receiverReg, args, argCount, dest, receiverIsConst);
          if (methodResult != 0xFFFF) {
            return methodResult;
          }
        }
      }
    }
  }

  // Check for enum method call (e.g., shape.distance(p))
  if (call.flags & FunctionCallFlags::IS_METHOD_CALL) {
    // Get the receiver object
    if (!call.moduleObject.IsNull()) {
      u16 receiverReg = LowerExpression(call.moduleObject);
      // registerStructTypes has MAX_REGISTERS entries. Constants and the
      // invalid marker can't have a valid struct-type hash; skip method
      // dispatch.
      if (!IR::IsRegister(receiverReg)) {
        return 0;
      }
      u32 receiverStructHash = program.registerStructTypes[receiverReg];

      if (receiverStructHash != 0) {
        // Look up the enum type by its struct hash
        // ENUM for module enums, ENUM_SYMBOL for pipeline-local enums.
        Symbol *enumSym = SymbolTable::LookupByHash(
            const_cast<SymbolTableData *>(symbols), receiverStructHash);
        const EnumData *resolvedEnumData = nullptr;
        if (enumSym && (enumSym->kind == SymbolKind::ENUM ||
                        enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
          resolvedEnumData = &symbols->enums[enumSym->index];
        } else {
          resolvedEnumData =
              SymbolTable::ResolveEnumDataByHash(symbols, receiverStructHash);
        }
        if (resolvedEnumData) {
          const EnumData &enumData = *resolvedEnumData;

          // Find the method by name
          NodeRef methodRef = NodeRef::Null();
          u32 methodModuleIndex = 0xFFFFFFFF;
          for (u32 e = 0; e < ast->enumDecls.count; e++) {
            const EnumDeclData &enumDecl = ast->enumDecls[e];
            if (enumDecl.name.nameHash != enumData.name.nameHash) {
              continue;
            }
            NodeRef enumRef(ASTNodeType::ENUM_DECL, e);
            for (u32 m = 0; m < ast->modules.count; m++) {
              const ModuleNodeData &module = ast->modules[m];
              for (u32 ei = 0; ei < module.enums.count; ei++) {
                if (module.enums[ei] == enumRef) {
                  methodModuleIndex = m;
                  break;
                }
              }
              if (methodModuleIndex != 0xFFFFFFFF) {
                break;
              }
            }
            for (u32 m = 0; m < enumDecl.methods.count; m++) {
              NodeRef mRef = enumDecl.methods[m];
              if (mRef.Type() == ASTNodeType::FUNCTION) {
                const FunctionDeclData &fn = ast->GetFunction(mRef);
                if (fn.name.nameHash == call.name.nameHash) {
                  methodRef = mRef;
                  break;
                }
              }
            }
            if (!methodRef.IsNull()) {
              break;
            }
          }

          for (u32 i = 0; i < enumData.methodIndices.count; i++) {
            if (!methodRef.IsNull())
              break;
            u32 funcIdx = enumData.methodIndices[i];
            if (funcIdx < symbols->functions.count) {
              const FunctionData &funcData = symbols->functions[funcIdx];
              if (funcData.returnType != CoreType::INVALID) {
                // Look up method in AST to check name
                for (u32 e = 0; e < ast->enumDecls.count; e++) {
                  const EnumDeclData &enumDecl = ast->enumDecls[e];
                  for (u32 m = 0; m < enumDecl.methods.count; m++) {
                    NodeRef mRef = enumDecl.methods[m];
                    if (mRef.Type() == ASTNodeType::FUNCTION) {
                      const FunctionDeclData &fn = ast->GetFunction(mRef);
                      if (fn.name.nameHash == call.name.nameHash) {
                        methodRef = mRef;
                        break;
                      }
                    }
                  }
                  if (!methodRef.IsNull())
                    break;
                }
              }
            }
            if (!methodRef.IsNull())
              break;
          }

          if (!methodRef.IsNull()) {
            // Get method body (should be a PatternMatch for eval methods)
            const FunctionDeclData &method = ast->GetFunction(methodRef);
            auto savedNodeRegisters = nodeRegisters;

            // Extract the enum tag for dispatch
            u16 tagReg = AllocateRegister();
            builder.EmitInstruction(OP_ENUM_TAG, tagReg, receiverReg);
            SetRegisterType(tagReg, CoreType::INT);

            // For now, emit a simplified dispatch using select operations
            // This avoids the complexity of full switch generation
            // We inline each variant case and use SELECT to pick result

            // Store arguments for use in inlined method body
            // Map 'self' to receiverReg and parameters to args[0..3]
            u16 selfReg = receiverReg;

            // If the method body is a PatternMatch, we need to handle it
            // For simplicity, generate a chain of comparisons + select
            if (method.body.Type() == ASTNodeType::PATTERN_MATCH) {
              const PatternMatchData &pm = ast->GetPatternMatch(method.body);
              auto savedVarRegs = variableRegisters;
              u32 selfHash = Utils::HashStr("self");
              variableRegisters[selfHash] = selfReg;

              // Bind method parameters that are not 'self' once for all arms
              u32 argIdx = 0;
              for (u32 p = 0; p < method.parameters.count && argIdx < 4;
                   p++) {
                const auto &param = method.parameters[p];
                if (param.second.nameHash != Utils::HashStr("self") &&
                    param.first.nameHash != 0) {
                  variableRegisters[param.first.nameHash] = CopyArgumentValue(args[argIdx++]);
                }
              }
              // Only the matching arm runs: it returns into an inline frame,
              // as an inlined function body does.
              if (inlineDepth >= MAX_INLINE_DEPTH) {
                variableRegisters = savedVarRegs;
                nodeRegisters = savedNodeRegisters;
                return ReportInlineDepthExceeded(ref);
              }
              for (u32 i = 0; i < inlineStackDepth; i++) {
                if (inlineStackPacked[i] != methodRef.packed) continue;
                if (!recursionDiagnosed) {
                  recursionDiagnosed = true;
                  ReportErrorAt(ref, "Error: recursion is not supported — a function "
                                     "calls itself (directly or through another "
                                     "function). SPIR-V execution is stack-less; "
                                     "rewrite the algorithm iteratively.\n");
                }
                variableRegisters = savedVarRegs;
                nodeRegisters = savedNodeRegisters;
                SetRegisterType(dest, CoreType::INVALID);
                return dest;
              }
              u16 resultReg = AllocateRegister();
              SetRegisterType(resultReg, method.returnType);
              builder.EmitInstruction(OP_STORE_REG, resultReg,
                                      EmitZeroConstant(method.returnType));
              if (method.returnType == CoreType::CUSTOM && method.returnTypeHash != 0) {
                u32 structHash = LookupOrRegisterStructType(method.returnTypeHash);
                program.registerStructTypes[resultReg] =
                    structHash != 0 ? structHash : method.returnTypeHash;
              }
              u16 savedReturnReg = inlineReturnReg;
              u16 savedReturnFlagReg = inlineReturnFlagReg;
              CoreType savedReturnType = inlineReturnType;
              u32 savedReturnStructHash = inlineReturnStructHash;
              u32 savedReturnCounter = inlineReturnCounter;
              u32 savedReturnFlagReads = inlineReturnFlagReads;
              u32 savedInlineModuleIndex = inlineModuleIndex;
              inlineReturnReg = resultReg;
              inlineReturnType = method.returnType;
              inlineReturnStructHash =
                  method.returnType == CoreType::CUSTOM ? method.returnTypeHash : 0;
              inlineReturnFlagReg = AllocateRegister();
              SetRegisterType(inlineReturnFlagReg, CoreType::BOOL);
              builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg,
                                      builder.EmitConstantBool(false));
              inlineReturnCounter = 0;
              inlineReturnFlagReads = 0;
              if (methodModuleIndex != 0xFFFFFFFF) inlineModuleIndex = methodModuleIndex;
              inlineDepth++;
              bool pushed = inlineStackDepth < MAX_INLINE_DEPTH;
              if (pushed) inlineStackPacked[inlineStackDepth++] = methodRef.packed;
              {
                DebugScopeGuard scope(builder, methodRef);
                LowerEnumPatternBody(method.body, selfReg, enumData);
              }
              if (pushed) inlineStackDepth--;
              inlineDepth--;
              inlineReturnReg = savedReturnReg;
              inlineReturnFlagReg = savedReturnFlagReg;
              inlineReturnType = savedReturnType;
              inlineReturnStructHash = savedReturnStructHash;
              inlineReturnCounter = savedReturnCounter;
              inlineReturnFlagReads = savedReturnFlagReads;
              inlineModuleIndex = savedInlineModuleIndex;

              variableRegisters = savedVarRegs;
              nodeRegisters = savedNodeRegisters;

              builder.EmitInstruction(OP_STORE_REG, dest, resultReg);
              SetRegisterType(dest, method.returnType);
              if (dest < MAX_REGISTERS) program.registerStructTypes[dest] = program.registerStructTypes[resultReg];
              return dest;
            } else {
              // Non-pattern-match method body (e.g., normal method that calls
              // self.distance) Inline the method body similar to regular
              // function inlining

              // Save variableRegisters to restore after inlining
              auto savedVarRegs = variableRegisters;

              // Bind method parameters to argument registers
              // param.first = name, param.second = type
              u32 argIdx = 0;
              for (u32 p = 0; p < method.parameters.count && argIdx < 4;
                   p++) {
                const auto &param = method.parameters[p];
                // Skip 'self' parameter (type is "self")
                if (param.second.nameHash != Utils::HashStr("self") &&
                    param.first.nameHash != 0) {
                  // Map parameter name to argument register
                  variableRegisters[param.first.nameHash] = CopyArgumentValue(args[argIdx++]);
                }
              }

              // Track self for nested method calls
              // Store the receiver in a way the nested calls can find it
              u32 selfHash = Utils::HashStr("self");
              variableRegisters[selfHash] = selfReg;

              // Check if method body is valid
              if (method.body.IsNull()) {
                // No body - return zero
                builder.EmitInstruction(OP_LOAD_CONST, dest,
                                        builder.EmitConstant(0.0f));
                SetRegisterType(dest, method.returnType);
                nodeRegisters = savedNodeRegisters;
                return dest;
              }

              // Evaluate the method body
              u32 savedInlineModuleIndex = inlineModuleIndex;
              if (methodModuleIndex != 0xFFFFFFFF) {
                inlineModuleIndex = methodModuleIndex;
              }
              u16 bodyResult = 0;
              {
                DebugScopeGuard scope(builder, methodRef);
                bodyResult = LowerExpression(method.body);
              }
              inlineModuleIndex = savedInlineModuleIndex;

              // Restore variableRegisters
              variableRegisters = savedVarRegs;
              nodeRegisters = savedNodeRegisters;

              // Store result
              builder.EmitInstruction(OP_STORE_REG, dest, bodyResult);
              SetRegisterType(dest, method.returnType);
              return dest;
            }
          }
        }
      }
    }
  }

  if (call.flags & FunctionCallFlags::IS_INTRINSIC) {
    if (ReportPointerArgumentToBuiltin(call, args, argCount)) {
      SetRegisterType(dest, CoreType::INVALID);
      return dest;
    }
    using Intrinsic = StdLib::Intrinsic;
    Intrinsic intrinsic =
        static_cast<Intrinsic>(StdLib::INTRINSICS[call.intrinsicIndex].enumIndex);

    OpCode op = OP_NOP;

    // A sampled texture or a sampler is a handle too, but only a storage
    // image can be written.
    if (intrinsic == Intrinsic::STORE && argCount > 0 &&
        GetResourceHandleType(call.arguments[0], args[0]) != CoreType::INVALID) {
      ReportErrorAt(call.arguments[0],
                    "Error: store() writes to a storage image; declare the "
                    "resource as image2D\n");
      SetRegisterType(dest, CoreType::INVALID);
      return dest;
    }

    switch (intrinsic) {
    case Intrinsic::ABS: {
      CoreType argType = GetRegisterType(args[0]);
      op = (mask(argType) & TypeMasks::FLOAT_TYPES) ? OP_FABS : OP_IABS;
      break;
    }
    case Intrinsic::MIN:
    case Intrinsic::MAX: {
        bool isMin = intrinsic == Intrinsic::MIN;
        CoreType argType = GetRegisterType(args[0]);
        // The opcode is picked from the first argument. The backends
        // splat scalars and convert int/uint, but not float/integer or
        // bool, so reject those mixes here rather than in SPIR-V
        // validation.
        constexpr TypeMask INTEGER_TYPES = TypeMasks::INT_TYPES | TypeMasks::UINT_TYPES;
        auto numericKind = [&](CoreType type) {
            if (mask(type) & TypeMasks::FLOAT_TYPES) return 1;
            if (mask(type) & INTEGER_TYPES) return 2;
            return 0;
        };
        int kind = numericKind(argType);
        for (u32 i = 0; i < argCount; i++) {
            CoreType type = GetRegisterType(args[i]);
            if (numericKind(type) == 0 || numericKind(type) != kind) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "Error: '%s' argument %u is %s, but argument 1 is %s; "
                         "use all float or all integer arguments\n",
                         isMin ? "min" : "max", i + 1, CoreTypeToString(type),
                         CoreTypeToString(argType));
                if (kind == 0)
                    snprintf(msg, sizeof(msg),
                             "Error: '%s' argument 1 is %s; expected a float "
                             "or integer scalar or vector\n",
                             isMin ? "min" : "max", CoreTypeToString(argType));
                ReportErrorAt(call.arguments[i], msg);
                return 0;
            }
        }
        if (kind == 1)
            op = isMin ? OP_FMIN : OP_FMAX;
        else if (mask(argType) & TypeMasks::UINT_TYPES)
            op = isMin ? OP_UMIN : OP_UMAX;
        else
            op = isMin ? OP_IMIN : OP_IMAX;
        // min/max are variadic: reduce left to right, min(a, b, c) =
        // min(min(a, b), c). The last step is emitted below like any binary
        // intrinsic, so clear consumed arguments out of its operand slots.
        for (u32 i = 2; i < argCount; i++) {
            u16 partial = AllocateRegister();
            builder.EmitInstruction(op, partial, args[0], args[1]);
            SetRegisterType(partial, argType);
            args[0] = partial;
            args[1] = args[i];
            args[i] = 0xFFFF;
        }
        break;
    }
    case Intrinsic::CLAMP: {
      CoreType argType = GetRegisterType(args[0]);
      if (mask(argType) & TypeMasks::FLOAT_TYPES)
        op = OP_FCLAMP;
      else if (mask(argType) & TypeMasks::UINT_TYPES)
        op = OP_UCLAMP;
      else
        op = OP_ICLAMP;
      break;
    }
    case Intrinsic::MOD: {
      CoreType argType = GetRegisterType(args[0]);
      op = (mask(argType) & TypeMasks::FLOAT_TYPES) ? OP_FMOD : OP_IMOD;
      break;
    }
    case Intrinsic::SINCOS: {
      u16 sinDest = dest;
      u16 cosDest = AllocateRegister();
      builder.EmitInstruction(OP_SIN, sinDest, args[0]);
      builder.EmitInstruction(OP_COS, cosDest, args[0]);
      return sinDest;
    }
    case Intrinsic::BARRIER:
    case Intrinsic::MEMORY_BARRIER:
    case Intrinsic::STORAGE_BARRIER: {
      if (currentStage != ShaderStage::Compute) {
        ReportError("Error: barrier intrinsics are only available in "
                    "compute shaders\n");
        return 0;
      }
      op = IntrinsicToOpcode(intrinsic);
      if (op != OP_NOP) {
        builder.EmitInstruction(op, dest, args[0], args[1], args[2]);
        SetRegisterType(dest, CoreType::VOID);
        return dest;
      }
      break;
    }
    case Intrinsic::ATOMIC_ADD:
    case Intrinsic::ATOMIC_MIN:
    case Intrinsic::ATOMIC_MAX:
    case Intrinsic::ATOMIC_AND:
    case Intrinsic::ATOMIC_OR:
    case Intrinsic::ATOMIC_XOR:
    case Intrinsic::ATOMIC_EXCHANGE:
    case Intrinsic::ATOMIC_CMP_EXCHANGE: {
      if (call.arguments.count > 0) {
        args[0] = LowerStoragePointerForAtomic(call.arguments[0]);
      }
      op = IntrinsicToOpcode(intrinsic);
      break;
    }
    // Texture operations need special handling:
    // sample(texture, coord)
    // sample(texture, sampler, coord)
    // sample_lod/bias/cmp(texture, [sampler,] coord, extra)
    // sample_*_offset(texture, [sampler,] coord, extra..., offset)
    // sample_grad(texture, [sampler,] coord, ddx, ddy)
    // Result type is always FLOAT4
    case Intrinsic::SAMPLE:
    case Intrinsic::SAMPLE_LOD:
    case Intrinsic::SAMPLE_BIAS:
    case Intrinsic::SAMPLE_GRAD:
    case Intrinsic::SAMPLE_CMP:
    case Intrinsic::SAMPLE_OFFSET:
    case Intrinsic::SAMPLE_LOD_OFFSET:
    case Intrinsic::SAMPLE_BIAS_OFFSET: {
      OpCode texOp = IntrinsicToOpcode(intrinsic);
      u16 texReg = args[0]; // A resource handle
      const bool hasExplicitSampler =
          argCount >= 3 && IsSamplerArgument(call.arguments[1], args[1]);
      u16 coordReg = hasExplicitSampler ? args[2] : args[1];

      switch (intrinsic) {
        case Intrinsic::SAMPLE:
          builder.EmitInstruction(texOp, dest, texReg, coordReg);
          break;
        case Intrinsic::SAMPLE_OFFSET: {
          u16 offsetReg = hasExplicitSampler ? args[3] : args[2];
          builder.EmitInstruction(texOp, dest, texReg, coordReg, offsetReg);
          break;
        }
        case Intrinsic::SAMPLE_LOD:
        case Intrinsic::SAMPLE_BIAS:
        case Intrinsic::SAMPLE_CMP: {
          u16 extraReg = hasExplicitSampler ? args[3] : args[2];
          builder.EmitInstruction(texOp, dest, texReg, coordReg, extraReg);
          break;
        }
        case Intrinsic::SAMPLE_LOD_OFFSET:
        case Intrinsic::SAMPLE_BIAS_OFFSET: {
          u16 extraReg = hasExplicitSampler ? args[3] : args[2];
          u16 offsetReg = hasExplicitSampler ? args[4] : args[3];
          builder.EmitInstruction(texOp, dest, texReg, coordReg, extraReg, offsetReg);
          break;
        }
        case Intrinsic::SAMPLE_GRAD: {
          u16 ddxReg = hasExplicitSampler ? args[3] : args[2];
          u16 ddyReg = hasExplicitSampler ? args[4] : args[3];
          builder.EmitInstruction(texOp, dest, texReg, coordReg, ddxReg, ddyReg);
          break;
        }
        default:
          break;
      }

      if (hasExplicitSampler) {
        BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                  builder.currentInstruction - 1,
                                                  static_cast<u16>(IR::ResourceHandleSlot(args[1])));
      }

      // Texture samples always return float4
      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::GATHER:
    case Intrinsic::GATHER_OFFSET: {
      OpCode texOp = IntrinsicToOpcode(intrinsic);
      u16 texReg = args[0]; // A resource handle
      const bool hasExplicitSampler =
          argCount >= 4 && IsSamplerArgument(call.arguments[1], args[1]);
      u16 coordReg = hasExplicitSampler ? args[2] : args[1];
      u16 componentReg = hasExplicitSampler ? args[3] : args[2];
      u16 offsetReg = IR::InvalidOperand;
      if (intrinsic == Intrinsic::GATHER_OFFSET) {
        offsetReg = hasExplicitSampler ? args[4] : args[3];
      }

      builder.EmitInstruction(texOp, dest, texReg, coordReg, componentReg, offsetReg);
      if (hasExplicitSampler) {
        BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                  builder.currentInstruction - 1,
                                                  static_cast<u16>(IR::ResourceHandleSlot(args[1])));
      }

      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::LOAD:
    case Intrinsic::LOAD_OFFSET: {
      OpCode texOp = IntrinsicToOpcode(intrinsic);
      u16 texReg = args[0];
      const bool hasExplicitSampler =
          argCount >= 4 && IsSamplerArgument(call.arguments[1], args[1]);
      u16 coordReg = hasExplicitSampler ? args[2] : args[1];
      u16 lodReg = hasExplicitSampler ? args[3] : args[2];
      u16 offsetReg = (intrinsic == Intrinsic::LOAD_OFFSET)
                          ? (hasExplicitSampler ? args[4] : args[3])
                          : IR::InvalidOperand;
      builder.EmitInstruction(texOp, dest, texReg, coordReg, lodReg, offsetReg);
      if (hasExplicitSampler) {
        BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                  builder.currentInstruction - 1,
                                                  static_cast<u16>(IR::ResourceHandleSlot(args[1])));
      }
      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::TEXTURE_SIZE: {
      u16 texReg = args[0];
      u16 lodReg = (argCount >= 2) ? args[1] : EmitConstantInt(0);
      builder.EmitInstruction(OP_TEX_SIZE, dest, texReg, lodReg);
      // Storage images have no handle type here; they are only 2D.
      SetRegisterType(dest, StdLib::TextureSizeType(
                                GetResourceHandleType(call.arguments[0], texReg)));
      return dest;
    }
    case Intrinsic::TEXTURE_LEVELS: {
      u16 texReg = args[0];
      builder.EmitInstruction(OP_TEX_LEVELS, dest, texReg);
      SetRegisterType(dest, CoreType::INT);
      return dest;
    }
    case Intrinsic::RCP: {
      u16 one = builder.EmitConstant(1.0f);
      builder.EmitInstruction(OP_FDIV, dest, one, args[0]);
      SetRegisterType(dest, GetRegisterType(args[0]));
      return dest;
    }
    case Intrinsic::LOG10: {
      u16 log2Reg = AllocateRegister();
      builder.EmitInstruction(OP_LOG2, log2Reg, args[0]);
      SetRegisterType(log2Reg, GetRegisterType(args[0]));
      u16 scale = builder.EmitConstant(0.3010299956639812f);
      builder.EmitInstruction(OP_FMUL, dest, log2Reg, scale);
      SetRegisterType(dest, GetRegisterType(args[0]));
      return dest;
    }
    case Intrinsic::F32TOF16: {
      u16 zero = builder.EmitConstant(0.0f);
      u16 pair = AllocateRegister();
      builder.EmitInstruction(OP_VEC_CONSTRUCT, pair, args[0], zero);
      SetRegisterType(pair, CoreType::FLOAT2);
      builder.EmitInstruction(OP_PACK_HALF2X16, dest, pair);
      SetRegisterType(dest, CoreType::UINT);
      return dest;
    }
    case Intrinsic::F16TOF32: {
      u16 pair = AllocateRegister();
      builder.EmitInstruction(OP_UNPACK_HALF2X16, pair, args[0]);
      SetRegisterType(pair, CoreType::FLOAT2);
      builder.EmitInstruction(OP_VEC_EXTRACT, dest, pair, 0);
      SetRegisterType(dest, CoreType::FLOAT);
      return dest;
    }
    case Intrinsic::MODF_SPLIT:
    case Intrinsic::FREXP: {
      // Same registration path as a declared BwslModfResult/BwslFrexpResult,
      // so call results and declared variables share one struct type.
      u32 structHash = LookupOrRegisterStructType(
          StdLib::FindBuiltinStructResult(intrinsic)->nameHash);
      builder.EmitInstruction(intrinsic == Intrinsic::FREXP ? OP_FREXP_STRUCT
                                                            : OP_MODF_STRUCT,
                              dest, args[0]);
      program.metadata[builder.currentInstruction - 1] = structHash;
      SetRegisterType(dest, CoreType::CUSTOM);
      program.registerStructTypes[dest] = structHash;
      return dest;
    }
    case Intrinsic::BITFIELD_INSERT: {
      builder.EmitInstruction(OP_BITFIELD_INSERT, dest, args[0], args[1],
                              args[2], args[3]);
      SetRegisterType(dest, GetRegisterType(args[0]));
      return dest;
    }
    case Intrinsic::SELECT: {
        // select(false_value, true_value, condition). The backends turn a
        // numeric condition into `!= 0` and don't check the value types, so
        // a swapped argument order would otherwise only fail SPIR-V
        // validation.
        if (argCount != 3) return 0; // The parser reported the count.
        CoreType falseType = GetRegisterType(args[0]);
        CoreType trueType = GetRegisterType(args[1]);
        CoreType condType = GetRegisterType(args[2]);
        u32 condWidth = (mask(condType) & TypeMasks::BOOL_VECTORS) ? GetVectorDimension(condType)
                      : condType == CoreType::BOOL                  ? 1
                                                                    : 0;
        // A value's type as the source names it: a struct by its name.
        auto valueTypeName = [&](u16 reg, CoreType type) {
            u32 hash = type == CoreType::CUSTOM ? program.registerStructTypes[reg] : 0;
            if (hash == 0) return SourceTypeName(type);
            std::string name = ReverseLookup::GetString(hash);
            return name.rfind("<hash:", 0) == 0 ? std::string("a struct") : name;
        };
        char msg[256];
        if (condWidth == 0) {
            snprintf(msg, sizeof(msg),
                     "Error: 'select' condition (argument 3) is %s, but must be bool; "
                     "the order is select(false_value, true_value, condition)\n",
                     SourceTypeName(condType).c_str());
            ReportErrorAt(call.arguments[2], msg);
            return 0;
        }
        if (falseType != trueType ||
            (falseType == CoreType::CUSTOM &&
             program.registerStructTypes[args[0]] != program.registerStructTypes[args[1]])) {
            std::string falseName = valueTypeName(args[0], falseType);
            std::string trueName = valueTypeName(args[1], trueType);
            if (falseName == trueName) {
                // Two structs that print alike, such as same-named structs
                // of different modules.
                snprintf(msg, sizeof(msg),
                         "Error: 'select' arguments 1 and 2 must have the same type, "
                         "but they are two different struct types\n");
            } else {
                snprintf(msg, sizeof(msg),
                         "Error: 'select' arguments 1 and 2 must have the same type, "
                         "but they are %s and %s\n",
                         falseName.c_str(), trueName.c_str());
            }
            ReportErrorAt(call.arguments[1], msg);
            return 0;
        }
        u32 valueWidth = GetVectorDimension(falseType);
        if (condWidth > 1 && condWidth != valueWidth) {
            snprintf(msg, sizeof(msg),
                     "Error: 'select' condition is %s, but the values are %s; "
                     "use a bool or a bool vector of the same width\n",
                     SourceTypeName(condType).c_str(), SourceTypeName(falseType).c_str());
            ReportErrorAt(call.arguments[2], msg);
            return 0;
        }
        op = OP_SELECT;
        break;
    }
    default:
      op = IntrinsicToOpcode(intrinsic);
      break;
    }

    if (op != OP_NOP) {
      builder.EmitInstruction(op, dest, args[0], args[1], args[2]);

      // Set the result type based on the intrinsic
      // Most intrinsics return the same type as their first argument
      // Exceptions: length, dot, distance return scalar float
      CoreType resultType = CoreType::FLOAT;
      switch (intrinsic) {
      case Intrinsic::LENGTH:
      case Intrinsic::DISTANCE:
      case Intrinsic::DOT:
      case Intrinsic::DETERMINANT:
        resultType = CoreType::FLOAT;
        break;
      case Intrinsic::ANY:
      case Intrinsic::ALL:
        resultType = CoreType::BOOL;
        break;
      case Intrinsic::STEP:
      case Intrinsic::SMOOTHSTEP:
        // Scalar edges apply to every component of x, the last argument.
        if (argCount > 0) {
          resultType = GetRegisterType(args[argCount - 1]);
        }
        break;
      case Intrinsic::AS_FLOAT: {
        CoreType argType = argCount > 0 ? GetRegisterType(args[0]) : CoreType::UINT;
        resultType = GetVectorType(CoreType::FLOAT, GetVectorDimension(argType));
        break;
      }
      case Intrinsic::AS_INT: {
        CoreType argType = argCount > 0 ? GetRegisterType(args[0]) : CoreType::FLOAT;
        resultType = GetVectorType(CoreType::INT, GetVectorDimension(argType));
        break;
      }
      case Intrinsic::AS_UINT: {
        CoreType argType = argCount > 0 ? GetRegisterType(args[0]) : CoreType::FLOAT;
        resultType = GetVectorType(CoreType::UINT, GetVectorDimension(argType));
        break;
      }
      case Intrinsic::PACK_UNORM4X8:
      case Intrinsic::PACK_UNORM2X16:
      case Intrinsic::PACK_SNORM4X8:
      case Intrinsic::PACK_SNORM2X16:
      case Intrinsic::PACK_HALF2X16:
        resultType = CoreType::UINT;
        break;
      case Intrinsic::UNPACK_UNORM2X16:
      case Intrinsic::UNPACK_SNORM2X16:
        resultType = CoreType::FLOAT2;
        break;
      case Intrinsic::UNPACK_UNORM4X8:
      case Intrinsic::UNPACK_SNORM4X8:
        resultType = CoreType::FLOAT4;
        break;
      case Intrinsic::UNPACK_HALF2X16:
        resultType = CoreType::FLOAT2;
        break;
      case Intrinsic::IS_NAN:
      case Intrinsic::IS_INF:
      case Intrinsic::IS_FINITE:
      case Intrinsic::IS_NORMAL: {
        // isnan/isinf return a bool (or bvec matching input width).
        CoreType argType = argCount > 0 ? GetRegisterType(args[0]) : CoreType::FLOAT;
        switch (argType) {
          case CoreType::FLOAT2: resultType = CoreType::BOOL2; break;
          case CoreType::FLOAT3: resultType = CoreType::BOOL3; break;
          case CoreType::FLOAT4: resultType = CoreType::BOOL4; break;
          default:               resultType = CoreType::BOOL;  break;
        }
        break;
      }
      default:
        // Most intrinsics return the same type as first arg
        if (argCount > 0) {
          resultType = GetRegisterType(args[0]);
        }
        break;
      }
      SetRegisterType(dest, resultType);
    }
  } else {
    // Check if it's a type constructor (float4, float3, int4, etc.)
    CoreType constructedType = LookupCoreType(call.name.nameHash);
    // GENERIC_T/U/V collide with short user struct names (`T`, `U`,
    // `V`). A user-defined struct/enum with that name should win, so
    // `V(a, b)` doesn't silently degrade into a vec-construct call.
    if (constructedType == CoreType::GENERIC_T ||
        constructedType == CoreType::GENERIC_U ||
        constructedType == CoreType::GENERIC_V) {
      Symbol *userSym = SymbolTable::LookupByHash(
          const_cast<SymbolTableData *>(symbols), call.name.nameHash);
      if (userSym && (userSym->kind == SymbolKind::CUSTOM_TYPE ||
                      userSym->kind == SymbolKind::ENUM ||
                      userSym->kind == SymbolKind::ENUM_SYMBOL)) {
        constructedType = CoreType::INVALID;
      }
    }
    if (constructedType != CoreType::INVALID &&
        constructedType != CoreType::VOID) {
      // A user function can intentionally use a core type word as its name
      // (for example, `double :: (int x) -> int`). Prefer a matching
      // overload before falling back to constructor/conversion semantics.
      u16 inlinedResult = TryInlineFunction(ref, call, args, argCount);
      if (inlinedResult != 0xFFFF) {
        builder.EmitInstruction(OP_STORE_REG, dest, inlinedResult);
        CoreType resultType = GetRegisterType(inlinedResult);
        if (resultType != CoreType::INVALID) {
          SetRegisterType(dest, resultType);
        }
        if ((resultType == CoreType::CUSTOM ||
             resultType == CoreType::ENUM) &&
            dest < MAX_REGISTERS && inlinedResult < MAX_REGISTERS) {
          u32 structHash = program.registerStructTypes[inlinedResult];
          if (structHash != 0) {
            program.registerStructTypes[dest] = structHash;
          }
        }
        return dest;
      }

      if (ReportPointerArgumentToBuiltin(call, args, argCount)) {
        SetRegisterType(dest, CoreType::INVALID);
        return dest;
      }

      // Check if this is a scalar type conversion (float(x), int(x), uint(x))
      bool isScalarConversion = (constructedType == CoreType::FLOAT ||
                                 constructedType == CoreType::INT ||
                                 constructedType == CoreType::UINT) &&
                                argCount == 1;

      if (isScalarConversion) {
        u16 constant = TryConvertScalarConstant(args[0], constructedType);
        if (constant != 0xFFFF) return constant;
        // Scalar type conversion - emit appropriate conversion opcode
        CoreType srcType = GetRegisterType(args[0]);
        if (srcType == constructedType) {
          return args[0];
        }
        OpCode convOp = OP_NOP;

        if (constructedType == CoreType::FLOAT) {
          if (srcType == CoreType::INT)
            convOp = OP_I2F;
          else if (srcType == CoreType::UINT)
            convOp = OP_U2F;
          else if (srcType == CoreType::BOOL) {
            // Bool to float: use select(0.0, 1.0, bool)
            // This becomes OpSelect in SPIR-V
            u16 zero = builder.EmitConstant(0.0f);
            u16 one = builder.EmitConstant(1.0f);
            builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
            SetRegisterType(dest, CoreType::FLOAT);
            return dest;
          } else
            convOp = OP_I2F; // Default to I2F
        } else if (constructedType == CoreType::INT) {
          if (srcType == CoreType::BOOL) {
            // Bool to int: use select(0, 1, bool)
            u16 zero = EmitConstantInt(0);
            u16 one = EmitConstantInt(1);
            builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
            SetRegisterType(dest, CoreType::INT);
            return dest;
          }
          if (srcType == CoreType::FLOAT)
            convOp = OP_F2I;
          else if (srcType == CoreType::UINT)
            convOp = OP_U2I;
          else
            convOp = OP_F2I;
        } else if (constructedType == CoreType::UINT) {
          if (srcType == CoreType::BOOL) {
            // Bool to uint: use select(0, 1, bool)
            u16 zero = EmitConstantUint(0);
            u16 one = EmitConstantUint(1);
            builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
            SetRegisterType(dest, CoreType::UINT);
            return dest;
          }
          if (srcType == CoreType::FLOAT)
            convOp = OP_F2U;
          else if (srcType == CoreType::INT)
            convOp = OP_I2U;
          else
            convOp = OP_F2U;
        }

        if (convOp != OP_NOP) {
          builder.EmitInstruction(convOp, dest, args[0]);
          SetRegisterType(dest, constructedType);
        }
      } else if (constructedType == CoreType::MAT2 ||
                 constructedType == CoreType::MAT3 ||
                 constructedType == CoreType::MAT4) {
        // Matrix type constructor
        // mat2 = 2 columns of vec2 (4 floats or 2 vec2s)
        // mat3 = 3 columns of vec3 (9 floats or 3 vec3s)
        // mat4 = 4 columns of vec4 (16 floats or 4 vec4s)
        u32 numColumns = (constructedType == CoreType::MAT2)   ? 2
                         : (constructedType == CoreType::MAT3) ? 3
                                                               : 4;
        u32 numRows = numColumns;
        CoreType columnType = (numColumns == 2)   ? CoreType::FLOAT2
                              : (numColumns == 3) ? CoreType::FLOAT3
                                                  : CoreType::FLOAT4;

        // Matrix-from-matrix conversion (GLSL semantics): a smaller target
        // truncates to the upper-left block, a larger target keeps the
        // source block and fills the rest from the identity matrix.
        if (argCount == 1) {
          CoreType srcMatType = GetRegisterType(args[0]);
          if (srcMatType == CoreType::MAT2 || srcMatType == CoreType::MAT3 ||
              srcMatType == CoreType::MAT4) {
            u32 srcColumns = (srcMatType == CoreType::MAT2)   ? 2
                             : (srcMatType == CoreType::MAT3) ? 3
                                                              : 4;
            CoreType srcColumnType = (srcColumns == 2)   ? CoreType::FLOAT2
                                     : (srcColumns == 3) ? CoreType::FLOAT3
                                                         : CoreType::FLOAT4;
            u16 columnRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            for (u32 col = 0; col < numColumns; col++) {
              if (col < srcColumns) {
                u16 srcCol = AllocateRegister();
                SetRegisterType(srcCol, srcColumnType);
                builder.EmitInstruction(OP_ARRAY_LOAD, srcCol, args[0],
                                        EmitConstantInt(col));
                if (numRows == srcColumns) {
                  columnRegs[col] = srcCol;
                } else if (numRows < srcColumns) {
                  // Shrink the column, keeping the first numRows lanes
                  u16 shrunk = AllocateRegister();
                  SetRegisterType(shrunk, columnType);
                  u32 shuffleMask = 0;
                  for (u32 i = 0; i < numRows; i++) {
                    shuffleMask |= (i & 0xF) << (i * 4);
                  }
                  builder.EmitInstruction(OP_VEC_SHUFFLE, shrunk, srcCol,
                                          srcCol);
                  program.metadata[builder.currentInstruction - 1] =
                      shuffleMask;
                  columnRegs[col] = shrunk;
                } else {
                  // Extend the column: source lanes, then identity padding
                  u16 lanes[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
                  for (u32 row = 0; row < numRows; row++) {
                    if (row < srcColumns) {
                      u16 lane = AllocateRegister();
                      SetRegisterType(lane, CoreType::FLOAT);
                      builder.EmitInstruction(OP_VEC_EXTRACT, lane, srcCol,
                                              (u16)row);
                      lanes[row] = lane;
                    } else {
                      lanes[row] =
                          builder.EmitConstant(col == row ? 1.0f : 0.0f);
                    }
                  }
                  u16 extended = AllocateRegister();
                  SetRegisterType(extended, columnType);
                  builder.EmitInstruction(OP_VEC_CONSTRUCT, extended, lanes[0],
                                          lanes[1], lanes[2], lanes[3]);
                  program.metadata[builder.currentInstruction - 1] = numRows;
                  columnRegs[col] = extended;
                }
              } else {
                // Identity column beyond the source matrix
                u16 lanes[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
                for (u32 row = 0; row < numRows; row++) {
                  lanes[row] = builder.EmitConstant(col == row ? 1.0f : 0.0f);
                }
                u16 ident = AllocateRegister();
                SetRegisterType(ident, columnType);
                builder.EmitInstruction(OP_VEC_CONSTRUCT, ident, lanes[0],
                                        lanes[1], lanes[2], lanes[3]);
                program.metadata[builder.currentInstruction - 1] = numRows;
                columnRegs[col] = ident;
              }
            }
            builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, columnRegs[0],
                                    columnRegs[1], columnRegs[2],
                                    columnRegs[3]);
            program.metadata[builder.currentInstruction - 1] = numColumns;
            SetRegisterType(dest, constructedType);
            return dest;
          }
        }

        // Single-scalar constructor builds a diagonal matrix, so that
        // mat4(1.0) is the identity matrix (GLSL semantics).
        if (argCount == 1) {
          CoreType srcScalarType = GetRegisterType(args[0]);
          if (srcScalarType == CoreType::FLOAT ||
              srcScalarType == CoreType::INT ||
              srcScalarType == CoreType::UINT) {
            u16 diag = args[0];
            if (srcScalarType != CoreType::FLOAT) {
              u16 conv = AllocateRegister();
              SetRegisterType(conv, CoreType::FLOAT);
              builder.EmitInstruction(
                  srcScalarType == CoreType::UINT ? OP_U2F : OP_I2F, conv,
                  diag);
              diag = conv;
            }
            u16 zero = builder.EmitConstant(0.0f);
            u16 columnRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            for (u32 col = 0; col < numColumns; col++) {
              u16 lanes[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
              for (u32 row = 0; row < numRows; row++) {
                lanes[row] = (row == col) ? diag : zero;
              }
              u16 colReg = AllocateRegister();
              SetRegisterType(colReg, columnType);
              builder.EmitInstruction(OP_VEC_CONSTRUCT, colReg, lanes[0],
                                      lanes[1], lanes[2], lanes[3]);
              program.metadata[builder.currentInstruction - 1] = numRows;
              columnRegs[col] = colReg;
            }
            builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, columnRegs[0],
                                    columnRegs[1], columnRegs[2],
                                    columnRegs[3]);
            program.metadata[builder.currentInstruction - 1] = numColumns;
            SetRegisterType(dest, constructedType);
            return dest;
          }
        }

        // Check if arguments are already column vectors (e.g., mat4(vec4,
        // vec4, vec4, vec4))
        bool argsAreColumnVectors = (argCount == numColumns);
        if (argsAreColumnVectors) {
          for (u32 i = 0; i < argCount; i++) {
            CoreType argType = GetRegisterType(args[i]);
            if (argType != columnType) {
              argsAreColumnVectors = false;
              break;
            }
          }
        }

        bool exactScalars = call.arguments.count == numColumns * numRows;
        if (exactScalars) {
          for (u32 i = 0; i < argCount; ++i) {
            CoreType type = GetRegisterType(args[i]);
            if (type != CoreType::FLOAT && type != CoreType::INT && type != CoreType::UINT)
              exactScalars = false;
          }
        }
        if (!argsAreColumnVectors && !exactScalars) {
          ReportError("Error: matrix constructor requires one scalar, one matrix, exact column vectors, or exactly rows*columns scalar arguments\n");
          SetRegisterType(dest, constructedType);
          return dest;
        }
        if (exactScalars) {
          for (u32 i = 0; i < argCount; ++i)
            args[i] = ConvertRegisterToType(args[i], CoreType::FLOAT);
        }
        u16 columnRegs[4];
        if (argsAreColumnVectors) {
          // Arguments are already column vectors - use them directly
          for (u32 col = 0; col < numColumns; col++) {
            columnRegs[col] = args[col];
          }
        } else {
          // Arguments are scalars - build column vectors
          for (u32 col = 0; col < numColumns; col++) {
            columnRegs[col] = AllocateRegister();
            SetRegisterType(columnRegs[col], columnType);

            // Get the scalars for this column
            u16 s0 = (col * numRows + 0 < argCount) ? args[col * numRows + 0]
                                                    : 0xFFFF;
            u16 s1 = (col * numRows + 1 < argCount) ? args[col * numRows + 1]
                                                    : 0xFFFF;
            u16 s2 = (numRows >= 3 && col * numRows + 2 < argCount)
                         ? args[col * numRows + 2]
                         : 0xFFFF;
            u16 s3 = (numRows >= 4 && col * numRows + 3 < argCount)
                         ? args[col * numRows + 3]
                         : 0xFFFF;

            builder.EmitInstruction(OP_VEC_CONSTRUCT, columnRegs[col], s0, s1,
                                    s2, s3);
            program.metadata[builder.currentInstruction - 1] = numRows;
          }
        }

        // Now construct the matrix from column vectors
        u16 c0 = columnRegs[0];
        u16 c1 = columnRegs[1];
        u16 c2 = (numColumns >= 3) ? columnRegs[2] : 0xFFFF;
        u16 c3 = (numColumns >= 4) ? columnRegs[3] : 0xFFFF;
        builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, c0, c1, c2, c3);
        program.metadata[builder.currentInstruction - 1] = numColumns;
        SetRegisterType(dest, constructedType);
      } else {
        // Validate component arity: a single scalar splats and a single
        // wider vector truncates; any other argument list must cover the
        // target component count exactly. Without this, float4(1.0, 2.0,
        // 3.0) silently zero-pads the missing lane.
        {
          u32 targetWidth = GetVectorDimension(constructedType);
          u32 providedComponents = 0;
          // An argument that already failed to lower (e.g. an unresolved
          // call) has no type and would count as one component, turning
          // its error into a misleading arity error as well.
          bool hasFailedArg = false;
          for (u32 i = 0; i < argCount; i++) {
            CoreType argType = GetRegisterType(args[i]);
            hasFailedArg |= argType == CoreType::INVALID;
            providedComponents += GetVectorDimension(argType);
          }
          if (hasFailedArg && hadError) {
            return 0;
          }
          bool validArity;
          if (argCount == 1) {
            validArity = providedComponents == 1 ||
                         providedComponents >= targetWidth;
          } else {
            validArity = providedComponents == targetWidth;
          }
          if (targetWidth > 1 && !validArity) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Error: vector constructor expects %u components, got "
                     "%u\n",
                     targetWidth, providedComponents);
            ReportError(msg);
            return 0;
          }
        }

        // Vector type constructor - emit OP_VEC_CONSTRUCT with up to 4
        // operands Use 0xFFFF as sentinel for unused operands
        u16 op0 = argCount > 0 ? args[0] : 0xFFFF;
        u16 op1 = argCount > 1 ? args[1] : 0xFFFF;
        u16 op2 = argCount > 2 ? args[2] : 0xFFFF;
        u16 op3 = argCount > 3 ? args[3] : 0xFFFF;
        builder.EmitInstruction(OP_VEC_CONSTRUCT, dest, op0, op1, op2, op3);
        // Store arg count in metadata for SPIR-V backend
        program.metadata[builder.currentInstruction - 1] = argCount;
        SetRegisterType(dest, constructedType);
        RecordConstantVector(dest);
      }
    } else {
      u32 callHash = call.name.nameHash;
      if (callHash == Utils::HashStr("float2x2") ||
          callHash == Utils::HashStr("float3x3") ||
          callHash == Utils::HashStr("float4x4")) {
        ReportError("Error: Matrix aliases float2x2/float3x3/float4x4 are not supported; use mat2, mat3, or mat4\n");
      }

      // Try to inline user function call
      u16 inlinedResult = TryInlineFunction(ref, call, args, argCount);
      if (inlinedResult != 0xFFFF) {
        // Successfully inlined - copy result to dest
        builder.EmitInstruction(OP_STORE_REG, dest, inlinedResult);
        // Inherit type from inlined result
        CoreType resultType = GetRegisterType(inlinedResult);
        if (resultType != CoreType::INVALID) {
          SetRegisterType(dest, resultType);
        }
        if ((resultType == CoreType::CUSTOM ||
             resultType == CoreType::ENUM) &&
            dest < MAX_REGISTERS && inlinedResult < MAX_REGISTERS) {
          u32 structHash = program.registerStructTypes[inlinedResult];
          if (structHash != 0) {
            program.registerStructTypes[dest] = structHash;
          }
        }
      } else {
        // Check for a user-struct positional constructor: `V(a, b, c)`
        // where V is a declared struct type. Previously fell through to
        // OP_CALL which emits OpUndef. For structs with up to 4 fields
        // we can fit the args into OP_STRUCT_CONSTRUCT directly (4
        // operand slots). For structs with more fields, start from an
        // undef composite and chain OP_STRUCT_INSERT per field so the
        // backend still produces a fully-populated OpCompositeInsert
        // sequence that matches the struct member count.
        u32 customHash = 0;
        CoreType resolved =
            ResolveCoreTypeFromHash(call.name.nameHash, &customHash);
        if (resolved == CoreType::CUSTOM && customHash != 0) {
          u32 structTypeHash = LookupOrRegisterStructType(customHash);
          if (structTypeHash != 0 &&
              ReportPointerArgumentToBuiltin(call, args, argCount)) {
            SetRegisterType(dest, CoreType::INVALID);
            return dest;
          }
          if (structTypeHash != 0) {
            auto it = structTypeMap.find(structTypeHash);
            u32 fieldCount = 0;
            if (it != structTypeMap.end()) {
              fieldCount = program.structTypes[it->second].fieldCount;
            }

            // `S(s)` with an S copies it.
            if (argCount == 1 && GetRegisterType(args[0]) == CoreType::CUSTOM &&
                IR::IsRegister(args[0]) &&
                LookupOrRegisterStructType(program.registerStructTypes[args[0]]) ==
                    structTypeHash) {
              return args[0];
            }
            if (it != structTypeMap.end() && argCount != fieldCount) {
              char msg[192];
              snprintf(msg, sizeof(msg),
                       "Error: struct %s constructor expects %u argument(s), "
                       "got %u\n",
                       ReverseLookup::GetString(structTypeHash).c_str(),
                       fieldCount, argCount);
              ReportErrorAt(ref, msg);
              SetRegisterType(dest, CoreType::INVALID);
              return dest;
            }
            if (it != structTypeMap.end()) {
              const IRProgram::StructTypeInfo &info = program.structTypes[it->second];
              for (u32 i = 0; i < argCount; i++) {
                u32 field = info.fieldOffset + i;
                CoreType fieldType = static_cast<CoreType>(program.structFieldTypes[field]);
                NodeRef argument = i < call.arguments.count ? call.arguments[i] : NodeRef::Null();
                if (program.structFieldArraySizes[field] == 0 &&
                    !CheckStoreShape(argument, args[i], fieldType,
                                     program.structFieldTypeHashes[field])) {
                  SetRegisterType(dest, CoreType::INVALID);
                  return dest;
                }
                if (program.structFieldArraySizes[field] == 0 &&
                    fieldType != CoreType::CUSTOM && fieldType != CoreType::ENUM)
                  args[i] = ConvertRegisterToType(args[i], fieldType);
              }
            }

            if (argCount <= 4 && fieldCount <= 4) {
              u16 op0 = argCount > 0 ? args[0] : 0xFFFF;
              u16 op1 = argCount > 1 ? args[1] : 0xFFFF;
              u16 op2 = argCount > 2 ? args[2] : 0xFFFF;
              u16 op3 = argCount > 3 ? args[3] : 0xFFFF;
              builder.EmitInstruction(OP_STRUCT_CONSTRUCT, dest, op0, op1,
                                      op2, op3);
              program.metadata[builder.currentInstruction - 1] =
                  structTypeHash;
              SetRegisterType(dest, CoreType::CUSTOM);
              if (dest < MAX_REGISTERS) {
                program.registerStructTypes[dest] = structTypeHash;
              }
              return dest;
            }

            // Struct has more than 4 fields (or user passed more than
            // 4 positional args). Build via undef-base + chained
            // STRUCT_INSERTs.
            u16 base = AllocateRegister();
            SetRegisterType(base, CoreType::CUSTOM);
            if (base < MAX_REGISTERS) {
              program.registerStructTypes[base] = structTypeHash;
            }
            AddUndefRegister(base, CoreType::CUSTOM);

            u16 current = base;
            u32 slots = fieldCount > 0 ? fieldCount : argCount;
            if (slots > argCount) slots = argCount;
            for (u32 i = 0; i < slots; i++) {
              u16 next = AllocateRegister();
              builder.EmitInstruction(OP_STRUCT_INSERT, next, current,
                                      static_cast<u16>(i), args[i]);
              program.metadata[builder.currentInstruction - 1] =
                  structTypeHash;
              SetRegisterType(next, CoreType::CUSTOM);
              if (next < MAX_REGISTERS) {
                program.registerStructTypes[next] = structTypeHash;
              }
              current = next;
            }

            // Copy the final composite into the originally-allocated
            // `dest` so subsequent code that references the call's
            // destination sees the fully-built struct.
            builder.EmitInstruction(OP_STORE_REG, dest, current);
            SetRegisterType(dest, CoreType::CUSTOM);
            if (dest < MAX_REGISTERS) {
              program.registerStructTypes[dest] = structTypeHash;
            }
            return dest;
          }
        }

        std::string functionName = call.name.isHashOnly()
            ? ReverseLookup::GetString(call.name.nameHash)
            : call.name.ToString(sourceBase);
        char msg[256];
        snprintf(msg, sizeof(msg), "Error: Function not found: %s\n",
                 functionName.c_str());
        ReportError(msg);
        // Leave the result untyped so enclosing constructors don't report
        // follow-on arity errors for it.
        SetRegisterType(dest, CoreType::INVALID);
      }
    }
  }

  return dest;
}

inline u16 IRLowering::TryLowerStructMethodCall(NodeRef ref,
                                                const FunctionCallData &call,
                                                u16 receiverReg, u16 *args,
                                                u32 argCount, u16 dest,
                                                bool receiverIsConst) {
  if (!IR::IsRegister(receiverReg)) {
    return 0xFFFF;
  }

  u32 receiverStructHash = program.registerStructTypes[receiverReg];
  if (receiverStructHash == 0) {
    return 0xFFFF;
  }

  StructData *structData = LookupStructDataByHash(receiverStructHash);
  if (!structData) {
    return 0xFFFF;
  }

  std::vector<OverloadTypeMask> argMasks;
  argMasks.reserve(argCount);
  for (u32 i = 0; i < argCount; i++) {
    argMasks.push_back(ArgumentOverloadMask(
        i < call.arguments.count ? call.arguments[i] : NodeRef::Null(), args[i]));
  }

  bool foundName = false;
  bool foundNonConstOnly = false;
  u32 methodIndex = SymbolTable::LookupStructMethodIndex(
      symbols, structData, call.name, argMasks.data(), argCount,
      receiverIsConst, &foundName, &foundNonConstOnly);

  if (methodIndex == INVALID_INDEX || methodIndex >= symbols->functions.count) {
    if (foundNonConstOnly && receiverIsConst) {
      ReportError(
          "Error: cannot call non-const struct method on const receiver\n");
    } else if (foundName && ReportStructMethodPointerMismatch(
                   structData, call, argMasks.data(), argCount)) {
      // Reported at the offending argument.
    } else if (foundName) {
      std::string methodName = call.name.isHashOnly()
                                   ? ReverseLookup::GetString(call.name.nameHash)
                                   : call.name.ToString(sourceBase);
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Error: no matching overload for struct method: %s\n",
               methodName.c_str());
      ReportError(msg);
    } else {
      std::string methodName = call.name.isHashOnly()
                                   ? ReverseLookup::GetString(call.name.nameHash)
                                   : call.name.ToString(sourceBase);
      char msg[256];
      snprintf(msg, sizeof(msg), "Error: unknown struct method: %s\n",
               methodName.c_str());
      ReportError(msg);
    }
    return dest;
  }

  const FunctionData &funcData = symbols->functions[methodIndex];
  if (!funcData.isStructMethod) {
    return 0xFFFF;
  }

  NodeRef methodRef;
  methodRef.packed = funcData.astNodeIndex;
  if (methodRef.Type() != ASTNodeType::FUNCTION) {
    return 0xFFFF;
  }

  const FunctionDeclData &method = ast->GetFunction(methodRef);
  if (method.body.IsNull()) {
    return 0xFFFF;
  }

  for (u32 i = 0; i < inlineStackDepth; i++) {
    if (inlineStackPacked[i] == methodRef.packed) {
      if (!recursionDiagnosed) {
        recursionDiagnosed = true;
        ReportError("Error: recursion is not supported — a function calls "
                    "itself (directly or through another function). SPIR-V "
                    "execution is stack-less; rewrite the algorithm "
                    "iteratively.\n");
      }
      return dest;
    }
  }

  if (inlineDepth >= MAX_INLINE_DEPTH) {
    return ReportInlineDepthExceeded(ref);
  }

  u32 methodModuleIndex = 0xFFFFFFFF;
  for (u32 m = 0; m < ast->modules.count; m++) {
    const ModuleNodeData &module = ast->modules[m];
    for (u32 s = 0; s < module.structs.count; s++) {
      NodeRef structRef = module.structs[s];
      if (structRef.Type() != ASTNodeType::STRUCT_DECL) {
        continue;
      }
      const StructDeclData &structDecl = ast->GetStructDecl(structRef);
      for (u32 i = 0; i < structDecl.methods.count; i++) {
        if (structDecl.methods[i] == methodRef) {
          methodModuleIndex = m;
          break;
        }
      }
      if (methodModuleIndex != 0xFFFFFFFF) {
        break;
      }
    }
    if (methodModuleIndex != 0xFFFFFFFF) {
      break;
    }
  }

  auto savedVariableRegisters = variableRegisters;
  auto savedVariableStructTypes = variableStructTypes;
  auto savedConstVariables = constVariables;
  auto savedInitializedVariables = initializedVariables;
  auto savedNodeRegisters = nodeRegisters;
  u32 savedStructMethodTypeHash = currentStructMethodTypeHash;
  u16 savedStructMethodSelfReg = currentStructMethodSelfReg;
  bool savedStructMethodIsConst = currentStructMethodIsConst;

  // The parameters and the body are the method's debug scope.
  DebugScopeGuard scope(builder, methodRef);

  u32 selfHash = Utils::HashStr("self");
  variableRegisters[selfHash] = receiverReg;
  variableStructTypes[selfHash] = receiverStructHash;
  // `self` aliases the receiver, which already holds a value. Without this,
  // the first `self.field = ...` would zero-initialize it and overwrite the
  // receiver's other fields.
  initializedVariables.insert(selfHash);
  if (receiverReg < MAX_REGISTERS) {
    program.registerStructTypes[receiverReg] = receiverStructHash;
  }
  if (method.isConstMethod) {
    constVariables.insert(selfHash);
  } else {
    constVariables.erase(selfHash);
  }

  u32 paramCount =
      method.parameters.count < argCount ? method.parameters.count : argCount;
  for (u32 i = 0; i < paramCount; i++) {
    if (IsPointerParameter(method, i)) {
      BindPointerParameter(call, method, i, args[i]);
      continue;
    }
    const auto &param = method.parameters[i];
    u32 paramNameHash = param.first.nameHash;
    u16 paramReg = CopyArgumentValue(args[i]);
    variableRegisters[paramNameHash] = paramReg;
    RecordParameterArrayDimensions(paramReg, method, i);
    constVariables.erase(paramNameHash);
    initializedVariables.insert(paramNameHash); // holds the argument's value
    program.registerNameHashes[paramReg] = paramNameHash; // its own variable

    u32 paramTypeHash = 0;
    CoreType paramType =
        ResolveCoreTypeFromHash(param.second.nameHash, &paramTypeHash);
    if (paramType != CoreType::INVALID && paramType != CoreType::VOID) {
      SetRegisterType(paramReg, paramType);
      if ((paramType == CoreType::CUSTOM || paramType == CoreType::ENUM) &&
          paramTypeHash != 0 && paramReg < MAX_REGISTERS) {
        u32 structHash = LookupOrRegisterStructType(paramTypeHash);
        if (structHash != 0) {
          program.registerStructTypes[paramReg] = structHash;
          variableStructTypes[paramNameHash] = structHash;
        }
      }
    }
  }

  u16 returnReg = AllocateRegister();
  u16 savedReturnReg = inlineReturnReg;
  u16 savedReturnFlagReg = inlineReturnFlagReg;
  CoreType savedReturnType = inlineReturnType;
  u32 savedReturnCounter = inlineReturnCounter;
  u32 savedReturnFlagReads = inlineReturnFlagReads;
  u32 savedModuleIndex = inlineModuleIndex;

  u32 savedReturnStructHash = inlineReturnStructHash;
  inlineReturnReg = returnReg;
  inlineReturnType = method.returnType;
  inlineReturnStructHash = method.returnType == CoreType::CUSTOM ? method.returnTypeHash : 0;
  inlineReturnFlagReg = AllocateRegister();
  SetRegisterType(inlineReturnFlagReg, CoreType::BOOL);
  u32 inlineFirstInstruction = builder.currentInstruction;
  builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg,
                          builder.EmitConstantBool(false));
  inlineReturnCounter = 0;
  inlineReturnFlagReads = 0;
  inlineModuleIndex = methodModuleIndex;
  inlineDepth++;
  if (inlineStackDepth < MAX_INLINE_DEPTH) {
    inlineStackPacked[inlineStackDepth++] = methodRef.packed;
  }

  currentStructMethodTypeHash = receiverStructHash;
  currentStructMethodSelfReg = receiverReg;
  currentStructMethodIsConst = method.isConstMethod;

  if (method.body.Type() == ASTNodeType::BLOCK) {
    LowerBlock(method.body);
  } else {
    u16 exprResult = LowerExpression(method.body);
    CheckStoreShape(method.body, exprResult, inlineReturnType, inlineReturnStructHash);
    builder.EmitInstruction(OP_STORE_REG, returnReg, exprResult);
  }
  builder.currentScope = scope.savedScope;  // What follows is the caller's

  if (inlineStackDepth > 0) {
    inlineStackDepth--;
  }
  inlineDepth--;
  DropUnreadReturnFlag(inlineFirstInstruction);
  inlineReturnReg = savedReturnReg;
  inlineReturnFlagReg = savedReturnFlagReg;
  inlineReturnType = savedReturnType;
  inlineReturnStructHash = savedReturnStructHash;
  inlineReturnCounter = savedReturnCounter;
  inlineReturnFlagReads = savedReturnFlagReads;
  inlineModuleIndex = savedModuleIndex;
  variableRegisters = savedVariableRegisters;
  variableStructTypes = savedVariableStructTypes;
  constVariables = savedConstVariables;
  initializedVariables = savedInitializedVariables;
  nodeRegisters = savedNodeRegisters;
  currentStructMethodTypeHash = savedStructMethodTypeHash;
  currentStructMethodSelfReg = savedStructMethodSelfReg;
  currentStructMethodIsConst = savedStructMethodIsConst;

  if (method.returnType != CoreType::INVALID &&
      method.returnType != CoreType::VOID) {
    SetRegisterType(returnReg, method.returnType);
    if ((method.returnType == CoreType::CUSTOM ||
         method.returnType == CoreType::ENUM) &&
        method.returnTypeHash != 0 && returnReg < MAX_REGISTERS) {
      u32 structHash = LookupOrRegisterStructType(method.returnTypeHash);
      if (structHash != 0) {
        program.registerStructTypes[returnReg] = structHash;
      }
    }
  } else if (method.returnType == CoreType::VOID) {
    SetRegisterType(returnReg, CoreType::VOID);
  }

  if (method.returnType == CoreType::VOID) {
    SetRegisterType(dest, CoreType::VOID);
    return dest;
  }

  builder.EmitInstruction(OP_STORE_REG, dest, returnReg);
  CoreType resultType = GetRegisterType(returnReg);
  if (resultType != CoreType::INVALID) {
    SetRegisterType(dest, resultType);
  }
  if ((resultType == CoreType::CUSTOM || resultType == CoreType::ENUM) &&
      dest < MAX_REGISTERS && returnReg < MAX_REGISTERS) {
    u32 structHash = program.registerStructTypes[returnReg];
    if (structHash != 0) {
      program.registerStructTypes[dest] = structHash;
    }
  }

  return dest;
}

inline u16 IRLowering::ReportInlineDepthExceeded(NodeRef ref) {
  if (!inlineDepthDiagnosed) {
    inlineDepthDiagnosed = true;
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Error: calls are nested more than %u deep here; merge some of "
             "the functions\n",
             MAX_INLINE_DEPTH);
    ReportErrorAt(ref, msg);
  }
  u16 placeholder = AllocateRegister();
  SetRegisterType(placeholder, CoreType::INVALID);
  return placeholder;
}

inline u16 IRLowering::TryInlineFunction(NodeRef ref, const FunctionCallData &call, u16 *args, u32 argCount) {
  // Look up the function in the AST
  // Check if this is a module-qualified call (e.g.,
  // Globals::decompressPosition)
  NodeRef funcRef;
  u32 foundModuleIndex =
      0xFFFFFFFF; // Track which module we found the function in
  std::vector<OverloadTypeMask> argMasks;
  argMasks.reserve(argCount);
  // Texture and sampler handles aren't real registers, so their type comes
  // from the argument expression; a texture handle would otherwise read as
  // a uint constant and match no texture2D parameter.
  CoreType argHandleTypes[16];
  for (u32 i = 0; i < argCount; i++) {
    argHandleTypes[i] = i < call.arguments.count
                            ? GetResourceHandleType(call.arguments[i], args[i])
                            : CoreType::INVALID;
    if (argHandleTypes[i] != CoreType::INVALID) {
      argMasks.push_back(MakeOverloadMask(argHandleTypes[i]));
      continue;
    }
    argMasks.push_back(ArgumentOverloadMask(
        i < call.arguments.count ? call.arguments[i] : NodeRef::Null(), args[i]));
  }

  // The first same-named candidate with the right arity that was rejected,
  // for a pointer-specific diagnostic when nothing matches.
  NodeRef rejectedCandidate = NodeRef::Null();
  auto matchesSignature = [&](const FunctionDeclData &fn, NodeRef fnRef) -> bool {
    if (fn.parameters.count != argCount)
      return false;
    for (u32 i = 0; i < argCount; i++) {
      OverloadTypeMask paramMask = MakeOverloadMaskFromResolvedTypeHash(
          fn.parameters[i].second.nameHash);
      if (IsPointerParameter(fn, i)) paramMask |= OVERLOAD_POINTER_MASK;
      if (!OverloadMaskMatches(paramMask, argMasks[i])) {
        // Overload rejection is normal during resolution — caller
        // tries multiple candidates. Downstream "Function not found"
        // error covers the truly-unresolvable case.
        if (rejectedCandidate.IsNull()) rejectedCandidate = fnRef;
        return false;
      }
    }
    return true;
  };

  u32 targetModuleIndex = call.moduleIndex;
  if ((targetModuleIndex == 0xFFFFFFFF ||
       targetModuleIndex >= ast->modules.count) &&
      !call.moduleObject.IsNull() &&
      call.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
    const IdentifierData &moduleIdent = ast->GetIdentifier(call.moduleObject);
    targetModuleIndex = SymbolTable::ResolveModuleIndexByHashInScope(
        const_cast<SymbolTableData *>(symbols), moduleIdent.name.nameHash,
        AliasOwnerKind(), AliasOwnerModuleIndex());
  }

  if (targetModuleIndex != 0xFFFFFFFF &&
      targetModuleIndex < ast->modules.count) {
    // Module-qualified call - search in the module's function list
    const ModuleNodeData &module = ast->modules[targetModuleIndex];

    for (u32 i = 0; i < module.functions.count; i++) {
      NodeRef fnRef = module.functions[i];
      if (fnRef.Type() == ASTNodeType::FUNCTION) {
        const FunctionDeclData &fn = ast->GetFunction(fnRef);
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn, fnRef)) {
          funcRef = fnRef;
          foundModuleIndex = targetModuleIndex;
          break;
        }
      }
    }
  }

  // Also try looking up by qualified hash in all modules
  if (funcRef.IsNull() && call.moduleQualifiedHash != 0) {
    for (u32 m = 0; m < ast->modules.count; m++) {
      const ModuleNodeData &module = ast->modules[m];

      for (u32 i = 0; i < module.functions.count; i++) {
        NodeRef fnRef = module.functions[i];
        if (fnRef.Type() == ASTNodeType::FUNCTION) {
          const FunctionDeclData &fn = ast->GetFunction(fnRef);
          if (fn.name.nameHash == call.moduleQualifiedHash) {
            if (!matchesSignature(fn, fnRef)) {
              continue;
            }
            funcRef = fnRef;
            foundModuleIndex = m;
            break;
          }
        }
      }
      if (!funcRef.IsNull())
        break;
    }
  }

  // For unqualified calls during inlining, search in the current inline
  // module
  if (funcRef.IsNull() && inlineModuleIndex != 0xFFFFFFFF &&
      inlineModuleIndex < ast->modules.count) {
    const ModuleNodeData &module = ast->modules[inlineModuleIndex];
    for (u32 i = 0; i < module.functions.count; i++) {
      NodeRef fnRef = module.functions[i];
      if (fnRef.Type() == ASTNodeType::FUNCTION) {
        const FunctionDeclData &fn = ast->GetFunction(fnRef);
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn, fnRef)) {
          funcRef = fnRef;
          foundModuleIndex = inlineModuleIndex;
          break;
        }
      }
    }
  }

  auto searchImportedModule = [&](u32 moduleIndex) {
    if (!funcRef.IsNull() || moduleIndex >= ast->modules.count) {
      return;
    }
    const ModuleNodeData &module = ast->modules[moduleIndex];
    for (u32 i = 0; i < module.functions.count; i++) {
      NodeRef fnRef = module.functions[i];
      if (fnRef.Type() == ASTNodeType::FUNCTION) {
        const FunctionDeclData &fn = ast->GetFunction(fnRef);
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn, fnRef)) {
          funcRef = fnRef;
          foundModuleIndex = moduleIndex;
          break;
        }
      }
    }
  };

  auto searchUsingList = [&](const ArenaArray<ArenaString> &usingImports) {
    for (u32 i = 0; i < usingImports.count && funcRef.IsNull(); i++) {
      u32 moduleIndex = SymbolTable::FindModuleByHash(
          const_cast<SymbolTableData *>(symbols), usingImports[i].nameHash);
      searchImportedModule(moduleIndex);
    }
  };

  if (funcRef.IsNull() && call.moduleObject.IsNull()) {
    if (inlineModuleIndex != 0xFFFFFFFF &&
        inlineModuleIndex < ast->modules.count) {
      searchUsingList(ast->modules[inlineModuleIndex].usingImports);
    } else if (!currentPipeline.IsNull()) {
      searchUsingList(ast->GetPipeline(currentPipeline).usingImports);
    }
  }

  // Try looking up in the current pass's function list (pass-scoped
  // functions)
  if (funcRef.IsNull() && !currentPass.IsNull()) {
    const PassData &pass = ast->GetPass(currentPass);
    for (u32 i = 0; i < pass.functions.count; i++) {
      NodeRef fnRef = pass.functions[i];
      if (fnRef.Type() == ASTNodeType::FUNCTION) {
        const FunctionDeclData &fn = ast->GetFunction(fnRef);
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn, fnRef)) {
          funcRef = fnRef;
          break;
        }
      }
    }
  }

  // Also try looking up in the current pipeline's function list
  if (funcRef.IsNull() && !currentPipeline.IsNull()) {
    const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.functions.count; i++) {
      NodeRef fnRef = pipeline.functions[i];
      if (fnRef.Type() == ASTNodeType::FUNCTION) {
        const FunctionDeclData &fn = ast->GetFunction(fnRef);
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn, fnRef)) {
          funcRef = fnRef;
          break;
        }
      }
    }
  }

  // If regular function not found, try generic function resolution
  if (funcRef.IsNull()) {
    funcRef = TryResolveGenericFunction(call, args, argCount);
  }

  if (funcRef.IsNull() && !rejectedCandidate.IsNull() &&
      ReportPointerArgumentMismatch(call, ast->GetFunction(rejectedCandidate),
                                    argMasks.data(), argCount)) {
    // Reported at the argument; an untyped result keeps the caller from
    // adding "Function not found" or follow-on type errors.
    u16 placeholder = AllocateRegister();
    SetRegisterType(placeholder, CoreType::INVALID);
    return placeholder;
  }

  if (funcRef.IsNull()) {
    // "Function not found" is a normal lookup outcome during
    // constructor calls and intrinsics — the parent dispatch has
    // its own handling for truly-unresolved symbols. Staying silent
    // here keeps end-user stderr clean.
    return 0xFFFF;
  }

  const FunctionDeclData &func = ast->GetFunction(funcRef);

  // Skip eval functions for now (they should be evaluated at compile time)
  if (func.isEval) {
    return 0xFFFF;
  }

  if (func.body.IsNull()) {
    return 0xFFFF; // No function body to inline
  }

  // Direct / indirect recursion check. SPIR-V has no call-stack
  // semantics — OpFunctionCall cannot target a caller in its own
  // chain. Previously recursive calls silently produced OpUndef
  // operands; validation then surfaced a cryptic "Expected int scalar
  // or vector type" error far from the root cause.
  for (u32 i = 0; i < inlineStackDepth; i++) {
    if (inlineStackPacked[i] == funcRef.packed) {
      if (!recursionDiagnosed) {
        recursionDiagnosed = true;
        ReportError("Error: recursion is not supported — a function calls "
                    "itself (directly or through another function). SPIR-V "
                    "execution is stack-less; rewrite the algorithm "
                    "iteratively.\n");
      }
      return 0xFFFF;
    }
  }

  if (inlineDepth >= MAX_INLINE_DEPTH) {
    return ReportInlineDepthExceeded(ref);
  }

  // Save current variable register mappings, struct types, and node register
  // cache Node register cache must be saved because we're re-lowering the
  // same AST nodes and each inlining needs fresh computations (not cached
  // results from previous inlining)
  auto savedVariableRegisters = variableRegisters;
  auto savedVariableStructTypes = variableStructTypes;
  auto savedConstVariables = constVariables;
  auto savedInitializedVariables = initializedVariables;
  auto savedResourceHandleVariables = resourceHandleVariables;
  auto savedNodeRegisters = nodeRegisters;
  u32 savedStructMethodTypeHash = currentStructMethodTypeHash;
  u16 savedStructMethodSelfReg = currentStructMethodSelfReg;
  bool savedStructMethodIsConst = currentStructMethodIsConst;
  currentStructMethodTypeHash = 0;
  currentStructMethodSelfReg = 0xFFFF;
  currentStructMethodIsConst = false;

  // The parameters and the body are the function's debug scope.
  DebugScopeGuard scope(builder, funcRef);

  // Bind parameters to argument registers
  u32 paramCount =
      func.parameters.count < argCount ? func.parameters.count : argCount;
  for (u32 i = 0; i < paramCount; i++) {
    const auto &param = func.parameters[i];
    u32 paramNameHash = param.first.nameHash; // Parameter name
    if (argHandleTypes[i] != CoreType::INVALID) {
      // Texture functions read the slot from the handle, so bind it as is.
      variableRegisters[paramNameHash] = args[i];
      resourceHandleVariables[paramNameHash] = {args[i], argHandleTypes[i]};
      constVariables.erase(paramNameHash);
      initializedVariables.insert(paramNameHash);
      continue;
    }
    if (IsPointerParameter(func, i)) {
      BindPointerParameter(call, func, i, args[i]);
      continue;
    }
    resourceHandleVariables.erase(paramNameHash);
    u16 paramReg = CopyArgumentValue(args[i]);
    variableRegisters[paramNameHash] = paramReg;
    RecordParameterArrayDimensions(paramReg, func, i);
    constVariables.erase(paramNameHash);
    initializedVariables.insert(paramNameHash); // holds the argument's value
    program.registerNameHashes[paramReg] = paramNameHash; // its own variable

    // Also set the type for the parameter based on the type name
    // The second element of the pair is the type name (e.g., "uint",
    // "float3")
    u32 paramTypeHash = 0;
    CoreType paramType =
        ResolveCoreTypeFromHash(param.second.nameHash, &paramTypeHash);

    if (paramType != CoreType::INVALID && paramType != CoreType::VOID) {
      SetRegisterType(paramReg, paramType);
      if ((paramType == CoreType::CUSTOM || paramType == CoreType::ENUM) &&
          paramTypeHash != 0) {
        u32 structHash = LookupOrRegisterStructType(paramTypeHash);

        if (structHash != 0) {
          program.registerStructTypes[paramReg] = structHash;
          // Also set variableStructTypes for local struct member access
          // (e.g., mat.albedo)
          variableStructTypes[paramNameHash] = structHash;
        }
      }
    }
  }

  // Create a register for the return value
  u16 returnReg = AllocateRegister();

  // Save the current return register and module index for nested inlining
  u16 savedReturnReg = inlineReturnReg;
  u16 savedReturnFlagReg = inlineReturnFlagReg;
  CoreType savedReturnType = inlineReturnType;
  u32 savedReturnCounter = inlineReturnCounter;
  u32 savedReturnFlagReads = inlineReturnFlagReads;
  u32 savedModuleIndex = inlineModuleIndex;
  u32 savedReturnStructHash = inlineReturnStructHash;
  inlineReturnReg = returnReg;
  inlineReturnType = func.returnType;
  inlineReturnStructHash = func.returnType == CoreType::CUSTOM ? func.returnTypeHash : 0;
  inlineReturnFlagReg = AllocateRegister();
  SetRegisterType(inlineReturnFlagReg, CoreType::BOOL);
  u32 inlineFirstInstruction = builder.currentInstruction;
  builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg,
                          builder.EmitConstantBool(false));
  inlineReturnCounter = 0;
  inlineReturnFlagReads = 0;
  inlineModuleIndex =
      foundModuleIndex; // Set module context for nested unqualified calls
  inlineDepth++;
  if (inlineStackDepth < MAX_INLINE_DEPTH) {
    inlineStackPacked[inlineStackDepth++] = funcRef.packed;
  }

  // Lower the function body
  if (func.body.Type() == ASTNodeType::PATTERN_MATCH) {
    const PatternMatchData &match = ast->GetPatternMatch(func.body);
    const EnumData *enumData = nullptr;
    for (u32 p = 0; p < func.parameters.count; ++p) {
      if (func.parameters[p].first.nameHash == match.scrutinee.nameHash) {
        enumData = SymbolTable::ResolveEnumDataByHash(symbols,
            SymbolTable::ResolveTypeAliasHashInScope(symbols, func.parameters[p].second.nameHash,
                                                     AliasOwnerKind(), AliasOwnerModuleIndex()));
        break;
      }
    }
    auto receiver = variableRegisters.find(match.scrutinee.nameHash);
    if (!enumData || receiver == variableRegisters.end()) {
      ReportError("Error: pattern function requires an enum argument\n");
    } else {
      SetRegisterType(returnReg, func.returnType);
      builder.EmitInstruction(OP_STORE_REG, returnReg, EmitZeroConstant(func.returnType));
      LowerEnumPatternBody(func.body, receiver->second, *enumData);
    }
  } else if (func.body.Type() == ASTNodeType::BLOCK) {
    LowerBlock(func.body);
  } else {
    // Single expression body
    u16 exprResult = LowerExpression(func.body);
    CheckStoreShape(func.body, exprResult, inlineReturnType, inlineReturnStructHash);
    builder.EmitInstruction(OP_STORE_REG, returnReg, exprResult);
  }
  builder.currentScope = scope.savedScope;  // What follows is the caller's

  // Restore state
  if (inlineStackDepth > 0) {
    inlineStackDepth--;
  }
  inlineDepth--;
  DropUnreadReturnFlag(inlineFirstInstruction);
  inlineReturnReg = savedReturnReg;
  inlineReturnFlagReg = savedReturnFlagReg;
  inlineReturnType = savedReturnType;
  inlineReturnStructHash = savedReturnStructHash;
  inlineReturnCounter = savedReturnCounter;
  inlineReturnFlagReads = savedReturnFlagReads;
  inlineModuleIndex = savedModuleIndex;
  variableRegisters = savedVariableRegisters;
  variableStructTypes = savedVariableStructTypes;
  constVariables = savedConstVariables;
  initializedVariables = savedInitializedVariables;
  resourceHandleVariables = savedResourceHandleVariables;
  nodeRegisters = savedNodeRegisters;
  currentStructMethodTypeHash = savedStructMethodTypeHash;
  currentStructMethodSelfReg = savedStructMethodSelfReg;
  currentStructMethodIsConst = savedStructMethodIsConst;

  // Set return type
  if (func.returnType != CoreType::INVALID &&
      func.returnType != CoreType::VOID) {
    SetRegisterType(returnReg, func.returnType);
  }

  return returnReg;
}

inline NodeRef IRLowering::TryResolveGenericFunction(const FunctionCallData &call, u16 *args,
                                  u32 argCount) {
  // Get argument types for constraint checking
  CoreType argTypes[16];
  bool isConstrained[16];
  bool argIsPointer[16];
  if (argCount > 16)
    return NodeRef::Null();

  for (u32 i = 0; i < argCount; i++) {
    // A pointer argument specializes a `Constraint^` parameter by its
    // pointee.
    argIsPointer[i] = IsPointerArgument(
        i < call.arguments.count ? call.arguments[i] : NodeRef::Null(), args[i]);
    argTypes[i] = argIsPointer[i]
        ? static_cast<CoreType>((program.registerStorageInfo[args[i]] >> 8) & 0xFF)
        : GetRegisterType(args[i]);
    isConstrained[i] = false; // Will be set based on generic function
  }

  // Find a matching generic function (symbols is const, need to cast for
  // lookup)
  SymbolTableData *mutableSymbols = const_cast<SymbolTableData *>(symbols);
  GenericFunctionData *gfn = SymbolTable::FindMatchingGenericFunction(
      mutableSymbols, call.name.nameHash, argTypes, argCount, argIsPointer);

  if (!gfn) {
    return NodeRef::Null(); // No matching generic function
  }

  // Build isConstrained array from the generic function's parameter info
  for (u32 i = 0; i < argCount && i < gfn->parameters.count; i++) {
    isConstrained[i] = gfn->parameters[i].isConstrained;
  }

  // Compute specialization hash. The matched template has the arguments'
  // pointer shape, which tells it apart from a same-named value template.
  u64 specHash = SpecializationRegistry::HashSpecialization(
      argTypes, isConstrained, argCount, argIsPointer);

  // Check if specialization already exists
  u32 existingFunc =
      mutableSymbols->specializationRegistry.Find(gfn->nameHash, specHash);
  if (existingFunc != UINT32_MAX) {
    // Return the existing specialized function's AST node
    // Look it up in the functions array
    for (u32 i = 0; i < ast->functions.count; i++) {
      NodeRef fnRef(ASTNodeType::FUNCTION, i);
      if (ast->GetFunction(fnRef).name.nameHash == existingFunc) {
        return fnRef;
      }
    }
  }

  // Need to instantiate a new specialization
  // Build type substitutions
  ASTClone::TypeSubstitution substitutions[16];
  u32 subCount = 0;

  for (u32 i = 0; i < gfn->parameters.count && i < argCount; i++) {
    if (gfn->parameters[i].isConstrained) {
      // Check if we already have this constraint in substitutions
      bool found = false;
      for (u32 j = 0; j < subCount; j++) {
        if (substitutions[j].constraintHash ==
            gfn->parameters[i].typeName.nameHash) {
          found = true;
          break;
        }
      }
      if (!found) {
        // Create ArenaString with the type's hash (hash-only, no source
        // backing)
        ArenaString typeStr;
        typeStr.nameHash = GetCoreTypeNameHash(argTypes[i]);
        typeStr.sourceOffset = 0;
        typeStr.nameLength = 0;

        substitutions[subCount].constraintHash =
            gfn->parameters[i].typeName.nameHash;
        substitutions[subCount].concreteType = typeStr;
        substitutions[subCount].coreType = argTypes[i];
        subCount++;
      }
    }
  }

  // Build mangled name
  std::string mangledName = SymbolTable::MangleSpecializationName(
      gfn->name, argTypes, isConstrained, argCount, argIsPointer);

  // Clone the generic function with type substitutions
  NodeRef srcFuncRef(ASTNodeType::FUNCTION, 0);
  // Find the source function by AST index
  for (u32 i = 0; i < ast->functions.count; i++) {
    if (NodeRef(ASTNodeType::FUNCTION, i).packed == gfn->astNodeIndex) {
      srcFuncRef = NodeRef(ASTNodeType::FUNCTION, i);
      break;
    }
  }

  if (srcFuncRef.IsNull()) {
    return NodeRef::Null();
  }

  NodeRef specializedFunc = ASTClone::CloneFunction(
      ast, ast->arena, srcFuncRef, substitutions, subCount, mangledName);

  if (specializedFunc.IsNull()) {
    return NodeRef::Null();
  }

  // Determine return type for the specialized function
  CoreType returnType = ast->GetFunction(srcFuncRef).returnType;
  if (gfn->returnMatchesParam >= 0 &&
      gfn->returnMatchesParam < static_cast<s8>(argCount)) {
    // Return type matches a parameter's type
    returnType = argTypes[gfn->returnMatchesParam];
  } else if (gfn->returnConstraint != 0) {
    // Return type is a constraint - use the first matching substitution
    for (u32 i = 0; i < subCount; i++) {
      if (substitutions[i].constraintHash == gfn->returnTypeName.nameHash) {
        returnType = substitutions[i].coreType;
        break;
      }
    }
  }

  // Update the specialized function's return type
  ast->GetFunction(specializedFunc).returnType = returnType;

  // Register the specialization
  mutableSymbols->specializationRegistry.Register(
      gfn->nameHash, specHash,
      ast->GetFunction(specializedFunc).name.nameHash);

  return specializedFunc;
}

inline u16 IRLowering::LowerTextureSample(NodeRef ref) {
    const FunctionCallData &call = ast->GetFunctionCall(ref);

    u16 texReg = LowerExpression(call.arguments[0]);
    u16 coordReg = LowerExpression(call.arguments[1]);
    u16 dest = AllocateRegister();

    // IR format: s0 = texture (a resource handle for the slot), s1 = coordinate
    if (IR::IsUintConstant(texReg)) {
        // texReg is already a resource handle, pass it directly
        builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
    } else {
        // Bindless - texReg is a register containing texture handle
        builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
    }

    return dest;
}

// ============= Texture arrays =============

inline const ResourceData *IRLowering::FindTextureArrayResource(NodeRef expr) const {
  if (expr.IsNull() || expr.Type() != ASTNodeType::MEMBER_ACCESS)
    return nullptr;
  const MemberAccessData &access = ast->GetMemberAccess(expr);
  if (access.object.IsNull() || access.object.Type() != ASTNodeType::IDENTIFIER ||
      ast->GetIdentifier(access.object).identifierKind != SpecialIdentifier::RESOURCES)
    return nullptr;
  Symbol *sym = SymbolTable::LookupByHash(const_cast<SymbolTableData *>(symbols),
                                          access.member.nameHash);
  if (!sym || sym->kind != SymbolKind::RESOURCE || sym->index >= symbols->resources.count)
    return nullptr;
  const ResourceData &resource = symbols->resources[sym->index];
  if (resource.type != ResourceBinding::Texture || resource.arraySize == 0)
    return nullptr;
  return &resource;
}

inline CoreType IRLowering::GetResourceHandleType(NodeRef expr, u16 reg) const {
    if (expr.IsNull())
        return CoreType::INVALID;
    if (expr.Type() == ASTNodeType::IDENTIFIER) {
        auto it = resourceHandleVariables.find(ast->GetIdentifier(expr).name.nameHash);
        // A later local with the same name rebinds the variable to another
        // register, so the handle must still match.
        if (it != resourceHandleVariables.end() && it->second.handle == reg)
            return it->second.type;
        return CoreType::INVALID;
    }
    if (expr.Type() == ASTNodeType::ARRAY_ACCESS) {
        const ResourceData* textures = FindTextureArrayResource(ast->GetArrayAccess(expr).array);
        return textures ? static_cast<CoreType>(textures->coreType) : CoreType::INVALID;
    }
    if (expr.Type() != ASTNodeType::MEMBER_ACCESS)
        return CoreType::INVALID;
    const MemberAccessData& access = ast->GetMemberAccess(expr);
    if (access.object.IsNull() || access.object.Type() != ASTNodeType::IDENTIFIER ||
        ast->GetIdentifier(access.object).identifierKind != SpecialIdentifier::RESOURCES)
        return CoreType::INVALID;
    Symbol* sym = SymbolTable::LookupByHash(const_cast<SymbolTableData*>(symbols),
                                            access.member.nameHash);
    if (!sym || sym->kind != SymbolKind::RESOURCE || sym->index >= symbols->resources.count)
        return CoreType::INVALID;
    const ResourceData& resource = symbols->resources[sym->index];
    // An unindexed texture array is not a handle; its access already reported.
    if ((resource.type == ResourceBinding::Texture && resource.arraySize == 0) ||
        resource.type == ResourceBinding::Sampler)
        return static_cast<CoreType>(resource.coreType);
    return CoreType::INVALID;
}

inline bool IRLowering::IsSamplerArgument(NodeRef argument, u16 reg) const {
    return GetResourceHandleType(argument, reg) == CoreType::SAMPLER;
}

inline u16 IRLowering::LowerTextureArrayElement(NodeRef ref) {
  const ArrayAccessData &access = ast->GetArrayAccess(ref);
  const ResourceData *textures = FindTextureArrayResource(access.array);
  const std::string name = ast->GetMemberAccess(access.array).member.ToString();
  if (textures->bindingIndex >= 32) {
    ReportErrorAt(ref, ("Error: texture array 'resources." + name +
                        "' has no binding slot; list it in the pass's 'use resources'\n").c_str());
    return 0;
  }
  s64 index = 0;
  if (!TryGetConstIndex(LowerExpression(access.index), access.index, &index)) {
    // TryLowerDynamicTextureArrayCall handles the supported dynamic case
    // before the argument is lowered.
    ReportErrorAt(ref, ("Error: texture array 'resources." + name +
                        "' indexed with a non-constant index can only be passed directly "
                        "to a texture function, e.g. sample(resources." + name + "[i], uv)\n").c_str());
    return 0;
  }
  if (index < 0 || index >= static_cast<s64>(textures->arraySize)) {
    ReportErrorAt(ref, ("Error: index " + std::to_string(index) + " is out of bounds for texture array 'resources." +
                        name + "' of length " + std::to_string(textures->arraySize) + "\n").c_str());
    return 0;
  }
  u16 texture = IR::MakeResourceHandle(static_cast<u32>(textures->bindingIndex + index));
  SetRegisterType(texture, static_cast<CoreType>(textures->coreType));
  return texture;
}

// GLSL ES 3.00 only indexes sampler arrays with constants, and Vulkan needs
// a dynamically uniform index, so a dynamic index becomes an if/else chain
// with one constant-index call per element, like a hand-written switch.
// Out-of-range indices take the last element.
inline bool IRLowering::TryLowerDynamicTextureArrayCall(NodeRef ref, u16 *result) {
  const FunctionCallData &call = ast->GetFunctionCall(ref);
  if (!(call.flags & FunctionCallFlags::IS_INTRINSIC) || call.arguments.count == 0)
    return false;
  NodeRef element = call.arguments[0];
  if (element.IsNull() || element.Type() != ASTNodeType::ARRAY_ACCESS ||
      nodeRegisters.count(element.packed))  // already inside an element's branch
    return false;
  const ArrayAccessData &access = ast->GetArrayAccess(element);
  const ResourceData *textures = FindTextureArrayResource(access.array);
  if (!textures || textures->bindingIndex >= 32)
    return false;
  u16 indexReg = LowerExpression(access.index);
  s64 constIndex;
  if (TryGetConstIndex(indexReg, access.index, &constIndex))
    return false;
  CoreType indexType = GetRegisterType(indexReg);
  if (indexType != CoreType::INT && indexType != CoreType::UINT) {
    ReportErrorAt(access.index, "Error: texture array index must be an int or uint scalar\n");
    *result = 0;
    return true;
  }

  // Lower the other arguments once, ahead of the branches, so every
  // element's call reuses them.
  for (u32 i = 1; i < call.arguments.count; i++)
    LowerExpression(call.arguments[i]);

  auto dominatingNodes = nodeRegisters;
  u16 selected = AllocateRegister();
  auto lowerElement = [&](u32 elementIndex) {
    nodeRegisters = dominatingNodes;
    nodeRegisters[element.packed] = IR::MakeResourceHandle(textures->bindingIndex + elementIndex);
    u16 value = LowerFunctionCall(ref);
    SetRegisterType(selected, GetRegisterType(value));
    builder.EmitInstruction(OP_STORE_REG, selected, value);
  };
  auto lowerFrom = [&](auto &&self, u32 elementIndex) -> void {
    if (elementIndex + 1 == textures->arraySize) {
      lowerElement(elementIndex);
      return;
    }
    u16 expected = indexType == CoreType::UINT ? EmitConstantUint(elementIndex)
                                               : EmitConstantInt(elementIndex);
    u16 equal = AllocateRegister();
    SetRegisterType(equal, CoreType::BOOL);
    builder.EmitInstruction(OP_IEQ, equal, indexReg, expected);
    u32 branch = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, equal);
    u32 trueTarget = builder.currentInstruction;
    lowerElement(elementIndex);
    u32 jump = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    u32 falseTarget = builder.currentInstruction;
    self(self, elementIndex + 1);
    u32 merge = builder.currentInstruction;
    builder.EmitInstruction(OP_NOP, 0, 0);
    program.SetBranchTargets(branch, trueTarget, falseTarget);
    program.structureInfo[branch] = IRProgram::PackStructure(IRProgram::STRUCT_IF_HEADER, merge);
    program.metadata[jump] = merge;
  };
  lowerFrom(lowerFrom, 0);
  nodeRegisters = std::move(dominatingNodes);
  *result = selected;
  return true;
}

inline OpCode IRLowering::IntrinsicToOpcode(StdLib::Intrinsic intrinsic) {
  using Intrinsic = StdLib::Intrinsic;
  switch (intrinsic) {
  // Math
  case Intrinsic::LERP:
    return OP_LERP;
  case Intrinsic::SMOOTHSTEP:
    return OP_SMOOTHSTEP;
  case Intrinsic::SATURATE:
    return OP_SATURATE;
  case Intrinsic::FRACT:
    return OP_FRACT;
  case Intrinsic::STEP:
    return OP_STEP;
  case Intrinsic::SIGN:
    return OP_SIGN;
  case Intrinsic::FLOOR:
    return OP_FLOOR;
  case Intrinsic::CEIL:
    return OP_CEIL;
  case Intrinsic::ROUND:
    return OP_ROUND;
  case Intrinsic::TRUNC:
    return OP_TRUNC;
  case Intrinsic::FMOD:
    return OP_FREM;
  case Intrinsic::FMA:
    return OP_FMA;
  case Intrinsic::POW:
    return OP_POW;
  case Intrinsic::SQRT:
    return OP_SQRT;
  case Intrinsic::RSQRT:
    return OP_RSQRT;
  case Intrinsic::EXP:
    return OP_EXP;
  case Intrinsic::EXP2:
    return OP_EXP2;
  case Intrinsic::LOG:
    return OP_LOG;
  case Intrinsic::LOG2:
    return OP_LOG2;
  case Intrinsic::LDEXP:
    return OP_LDEXP;

  // Trigonometry
  case Intrinsic::SIN:
    return OP_SIN;
  case Intrinsic::COS:
    return OP_COS;
  case Intrinsic::TAN:
    return OP_TAN;
  case Intrinsic::ASIN:
    return OP_ASIN;
  case Intrinsic::ACOS:
    return OP_ACOS;
  case Intrinsic::ATAN:
    return OP_ATAN;
  case Intrinsic::ATAN2:
    return OP_ATAN2;
  case Intrinsic::SINH:
    return OP_SINH;
  case Intrinsic::COSH:
    return OP_COSH;
  case Intrinsic::TANH:
    return OP_TANH;
  case Intrinsic::DEGREES:
    return OP_DEGREES;
  case Intrinsic::RADIANS:
    return OP_RADIANS;

  // Vector
  case Intrinsic::DOT:
    return OP_DOT;
  case Intrinsic::CROSS:
    return OP_CROSS;
  case Intrinsic::NORMALIZE:
    return OP_NORMALIZE;
  case Intrinsic::LENGTH:
    return OP_LENGTH;
  case Intrinsic::DISTANCE:
    return OP_DISTANCE;
  case Intrinsic::REFLECT:
    return OP_REFLECT;
  case Intrinsic::REFRACT:
    return OP_REFRACT;
  case Intrinsic::FACEFORWARD:
    return OP_FACEFORWARD;

  // Matrix
  case Intrinsic::TRANSPOSE:
    return OP_MAT_TRANSPOSE;
  case Intrinsic::DETERMINANT:
    return OP_MAT_DET;
  case Intrinsic::INVERSE:
    return OP_MAT_INVERSE;

  // Derivatives (fragment only)
  case Intrinsic::DDX:
    return OP_DDX;
  case Intrinsic::DDY:
    return OP_DDY;
  case Intrinsic::DDX_FINE:
    return OP_DDX_FINE;
  case Intrinsic::DDY_FINE:
    return OP_DDY_FINE;
  case Intrinsic::DDX_COARSE:
    return OP_DDX_COARSE;
  case Intrinsic::DDY_COARSE:
    return OP_DDY_COARSE;
  case Intrinsic::FWIDTH:
    return OP_FWIDTH;
  case Intrinsic::FWIDTH_FINE:
    return OP_FWIDTH_FINE;
  case Intrinsic::FWIDTH_COARSE:
    return OP_FWIDTH_COARSE;

  // Texture operations - these need special handling
  case Intrinsic::SAMPLE:
    return OP_TEX_SAMPLE;
  case Intrinsic::SAMPLE_LOD:
    return OP_TEX_SAMPLE_LOD;
  case Intrinsic::SAMPLE_BIAS:
    return OP_TEX_SAMPLE_BIAS;
  case Intrinsic::SAMPLE_GRAD:
    return OP_TEX_SAMPLE_GRAD;
  case Intrinsic::SAMPLE_CMP:
    return OP_TEX_SAMPLE_CMP;
  case Intrinsic::SAMPLE_OFFSET:
    return OP_TEX_SAMPLE_OFFSET;
  case Intrinsic::SAMPLE_LOD_OFFSET:
    return OP_TEX_SAMPLE_LOD_OFFSET;
  case Intrinsic::SAMPLE_BIAS_OFFSET:
    return OP_TEX_SAMPLE_BIAS_OFFSET;
  case Intrinsic::GATHER:
    return OP_TEX_GATHER;
  case Intrinsic::GATHER_OFFSET:
    return OP_TEX_GATHER_OFFSET;
  case Intrinsic::LOAD:
    return OP_TEX_FETCH;
  case Intrinsic::LOAD_OFFSET:
    return OP_TEX_FETCH_OFFSET;
  case Intrinsic::STORE:
    return OP_IMG_STORE;

  // Synchronization
  case Intrinsic::BARRIER:
    return OP_BARRIER;
  case Intrinsic::MEMORY_BARRIER:
    return OP_MEM_FENCE;
  case Intrinsic::STORAGE_BARRIER:
    return OP_MEM_FENCE;

  // Wave/SIMD operations
  case Intrinsic::WAVE_ACTIVE_SUM:
    return OP_WAVE_SUM;
  case Intrinsic::WAVE_ACTIVE_PRODUCT:
    return OP_WAVE_MUL;
  case Intrinsic::WAVE_ACTIVE_MIN:
    return OP_WAVE_MIN;
  case Intrinsic::WAVE_ACTIVE_MAX:
    return OP_WAVE_MAX;
  case Intrinsic::WAVE_ACTIVE_ALL:
    return OP_WAVE_ALL;
  case Intrinsic::WAVE_ACTIVE_ANY:
    return OP_WAVE_ANY;
  case Intrinsic::WAVE_BROADCAST:
    return OP_WAVE_READ_LANE;
  case Intrinsic::WAVE_READ_FIRST:
    return OP_WAVE_READ_FIRST;

  // Atomics
  case Intrinsic::ATOMIC_ADD:
    return OP_ATOMIC_ADD;
  case Intrinsic::ATOMIC_MIN:
    return OP_ATOMIC_MIN;
  case Intrinsic::ATOMIC_MAX:
    return OP_ATOMIC_MAX;
  case Intrinsic::ATOMIC_AND:
    return OP_ATOMIC_AND;
  case Intrinsic::ATOMIC_OR:
    return OP_ATOMIC_OR;
  case Intrinsic::ATOMIC_XOR:
    return OP_ATOMIC_XOR;
  case Intrinsic::ATOMIC_EXCHANGE:
    return OP_ATOMIC_XCHG;
  case Intrinsic::ATOMIC_CMP_EXCHANGE:
    return OP_ATOMIC_CMP_XCHG;

  // Bit operations
  case Intrinsic::COUNT_BITS:
    return OP_POPCNT;
  case Intrinsic::REVERSE_BITS:
    return OP_REVERSE_BITS;
  case Intrinsic::FIRST_BIT_LOW:
    return OP_CTZ;
  case Intrinsic::FIRST_BIT_HIGH:
    return OP_CLZ;
  case Intrinsic::BITFIELD_EXTRACT:
    return OP_BITFIELD_EXTRACT;
  case Intrinsic::BITFIELD_INSERT:
    return OP_BITFIELD_INSERT;
  case Intrinsic::PACK_UNORM2X16:
    return OP_PACK_UNORM2X16;
  case Intrinsic::UNPACK_UNORM2X16:
    return OP_UNPACK_UNORM2X16;
  case Intrinsic::PACK_UNORM4X8:
    return OP_PACK_UNORM4X8;
  case Intrinsic::UNPACK_UNORM4X8:
    return OP_UNPACK_UNORM4X8;
  case Intrinsic::PACK_SNORM2X16:
    return OP_PACK_SNORM2X16;
  case Intrinsic::UNPACK_SNORM2X16:
    return OP_UNPACK_SNORM2X16;
  case Intrinsic::PACK_SNORM4X8:
    return OP_PACK_SNORM4X8;
  case Intrinsic::UNPACK_SNORM4X8:
    return OP_UNPACK_SNORM4X8;
  case Intrinsic::PACK_HALF2X16:
    return OP_PACK_HALF2X16;
  case Intrinsic::UNPACK_HALF2X16:
    return OP_UNPACK_HALF2X16;
  case Intrinsic::TEXTURE_LEVELS:
    return OP_TEX_LEVELS;
  case Intrinsic::AS_FLOAT:
  case Intrinsic::AS_INT:
  case Intrinsic::AS_UINT:
    return OP_BITCAST;

  // Control flow
  case Intrinsic::SELECT:
    return OP_SELECT;

  // Boolean reductions
  case Intrinsic::ANY:
    return OP_ANY;
  case Intrinsic::ALL:
    return OP_ALL;

  // Float classification
  case Intrinsic::IS_NAN:
    return OP_ISNAN;
  case Intrinsic::IS_INF:
    return OP_ISINF;
  case Intrinsic::IS_FINITE:
    return OP_ISFINITE;
  case Intrinsic::IS_NORMAL:
    return OP_ISNORMAL;

  default:
    return OP_NOP;
  }
}


} // namespace BWSL::IR
