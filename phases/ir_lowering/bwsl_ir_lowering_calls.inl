// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.

inline u16 IRLowering::LowerFunctionCall(NodeRef ref) {
  const FunctionCallData &call = ast->GetFunctionCall(ref);

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
        // Register the enum as a struct type if not already done
        u32 enumStructHash =
            LookupOrRegisterStructType(enumData.name.nameHash);

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

  // Check for enum method call (e.g., shape.distance(p))
  if (call.flags & FunctionCallFlags::IS_METHOD_CALL) {
    // Get the receiver object
    if (!call.moduleObject.IsNull()) {
      u16 receiverReg = LowerExpression(call.moduleObject);
      // registerStructTypes has MAX_REGISTERS entries. Constant-encoded
      // registers (bits 0x4000 / 0x8000) and any reg past MAX_REGISTERS
      // can't have a valid struct-type hash; skip method dispatch.
      if ((receiverReg & 0xC000) != 0 || receiverReg >= MAX_REGISTERS) {
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
                  variableRegisters[param.first.nameHash] = args[argIdx++];
                }
              }
              auto baseVarRegs = variableRegisters;

              // For each arm, check if tag == variantIndex, evaluate body,
              // select
              u16 resultReg = AllocateRegister();
              SetRegisterType(resultReg, method.returnType);

              u16 initValue = EmitZeroConstant(method.returnType);
              builder.EmitInstruction(OP_STORE_REG, resultReg, initValue);

              u16 matchedReg = builder.EmitConstantBool(false);
              u32 underscoreHash = Utils::HashStr("_");

              for (u32 armIdx = 0; armIdx < pm.arms.count; armIdx++) {
                NodeRef armRef = pm.arms[armIdx];
                if (armRef.Type() != ASTNodeType::PATTERN_MATCH_ARM) {
                  continue;
                }
                const PatternMatchData &arm = ast->GetPatternMatch(armRef);

                u16 condReg = 0xFFFF;
                u32 variantIdx = 0xFFFFFFFF;
                if (!arm.isDefault) {
                  for (u32 v = 0; v < enumData.variants.count; v++) {
                    if (enumData.variants[v].name.nameHash ==
                        arm.variantHash) {
                      variantIdx = v;
                      break;
                    }
                  }
                  if (variantIdx == 0xFFFFFFFF) {
                    continue;
                  }
                  u16 expectedTag = EmitConstantInt(variantIdx);
                  condReg = AllocateRegister();
                  builder.EmitInstruction(OP_IEQ, condReg, tagReg,
                                          expectedTag);
                  SetRegisterType(condReg, CoreType::BOOL);

                  u16 newMatched = AllocateRegister();
                  builder.EmitInstruction(OP_OR, newMatched, matchedReg,
                                          condReg);
                  SetRegisterType(newMatched, CoreType::BOOL);
                  matchedReg = newMatched;
                } else {
                  u16 notMatched = AllocateRegister();
                  builder.EmitInstruction(OP_NOT, notMatched, matchedReg);
                  SetRegisterType(notMatched, CoreType::BOOL);
                  condReg = notMatched;
                }

                variableRegisters = baseVarRegs;

                if (variantIdx != 0xFFFFFFFF) {
                  const EnumData::Variant &variant =
                      enumData.variants[variantIdx];
                  u32 payloadIndex = 0;
                  u32 bindingIdx = 0;

                  for (u32 t = 0; t < variant.associatedTypes.count; t++) {
                    CoreType assocType = variant.associatedTypes[t];
                    u32 assocHash = 0;
                    if (t < variant.associatedTypeHashes.count) {
                      assocHash = variant.associatedTypeHashes[t];
                    }
                    u32 componentCount =
                        GetEnumPayloadFieldCount(assocType, assocHash);
                    if (bindingIdx >= arm.bindings.count) {
                      payloadIndex += componentCount;
                      continue;
                    }

                    const auto &binding = arm.bindings[bindingIdx++];
                    if (binding.first.nameHash == underscoreHash) {
                      payloadIndex += componentCount;
                      continue;
                    }

                    bool isNestedSumEnum =
                        assocType == CoreType::CUSTOM && assocHash != 0 &&
                        IsSumEnumTypeHash(assocHash);

                    if (isNestedSumEnum) {
                      u16 fieldReg = AllocateRegister();
                      u16 fieldIdx = EmitConstantInt(payloadIndex);
                      builder.EmitInstruction(OP_ENUM_FIELD, fieldReg,
                                              selfReg, fieldIdx);
                      SetRegisterType(fieldReg, CoreType::CUSTOM);
                      u32 structHash = LookupOrRegisterStructType(assocHash);
                      program.registerStructTypes[fieldReg] =
                          structHash != 0 ? structHash : assocHash;
                      variableRegisters[binding.first.nameHash] = fieldReg;
                    } else if (componentCount == 1) {
                      u16 fieldReg = AllocateRegister();
                      u16 fieldIdx = EmitConstantInt(payloadIndex);
                      builder.EmitInstruction(OP_ENUM_FIELD, fieldReg,
                                              selfReg, fieldIdx);
                      SetRegisterType(fieldReg, assocType);
                      if (assocType == CoreType::CUSTOM && assocHash != 0) {
                        u32 structHash = LookupOrRegisterStructType(assocHash);
                        program.registerStructTypes[fieldReg] =
                            structHash != 0 ? structHash : assocHash;
                      }
                      variableRegisters[binding.first.nameHash] = fieldReg;
                    } else {
                      if (assocType == CoreType::CUSTOM) {
                        ReportError("Error: binding flattened struct enum "
                                    "payloads is not supported yet\n");
                        payloadIndex += componentCount;
                        continue;
                      }
                      u16 components[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
                      CoreType scalarType = GetScalarComponentType(assocType);
                      for (u32 c = 0; c < componentCount && c < 4; c++) {
                        u16 compReg = AllocateRegister();
                        u16 compIdx = EmitConstantInt(payloadIndex + c);
                        builder.EmitInstruction(OP_ENUM_FIELD, compReg,
                                                selfReg, compIdx);
                        SetRegisterType(compReg, scalarType);
                        components[c] = compReg;
                      }

                      u16 vecReg = AllocateRegister();
                      builder.EmitInstruction(OP_VEC_CONSTRUCT, vecReg,
                                              components[0], components[1],
                                              components[2], components[3]);
                      program.metadata[builder.currentInstruction - 1] =
                          componentCount;
                      SetRegisterType(vecReg, assocType);
                      variableRegisters[binding.first.nameHash] = vecReg;
                    }

                    payloadIndex += componentCount;
                  }
                }

                u16 armResult = 0;
                if (!arm.body.IsNull()) {
                  u32 savedInlineModuleIndex = inlineModuleIndex;
                  if (methodModuleIndex != 0xFFFFFFFF) {
                    inlineModuleIndex = methodModuleIndex;
                  }
                  armResult = LowerExpression(arm.body);
                  inlineModuleIndex = savedInlineModuleIndex;
                }

                u16 newResult = AllocateRegister();
                builder.EmitInstruction(OP_SELECT, newResult, resultReg,
                                        armResult, condReg);
                SetRegisterType(newResult, method.returnType);
                resultReg = newResult;
              }

              variableRegisters = savedVarRegs;
              nodeRegisters = savedNodeRegisters;

              builder.EmitInstruction(OP_STORE_REG, dest, resultReg);
              SetRegisterType(dest, method.returnType);
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
                  variableRegisters[param.first.nameHash] = args[argIdx++];
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
              u16 bodyResult = LowerExpression(method.body);
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
    using Intrinsic = StdLib::Intrinsic;
    Intrinsic intrinsic =
        static_cast<Intrinsic>(StdLib::INTRINSICS[call.intrinsicIndex].enumIndex);

    OpCode op = OP_NOP;

    switch (intrinsic) {
    case Intrinsic::ABS: {
      CoreType argType = GetRegisterType(args[0]);
      op = (mask(argType) & TypeMasks::FLOAT_TYPES) ? OP_FABS : OP_IABS;
      break;
    }
    case Intrinsic::MIN: {
      CoreType argType = GetRegisterType(args[0]);
      if (mask(argType) & TypeMasks::FLOAT_TYPES)
        op = OP_FMIN;
      else if (mask(argType) & TypeMasks::UINT_TYPES)
        op = OP_UMIN;
      else
        op = OP_IMIN;
      break;
    }
    case Intrinsic::MAX: {
      CoreType argType = GetRegisterType(args[0]);
      if (mask(argType) & TypeMasks::FLOAT_TYPES)
        op = OP_FMAX;
      else if (mask(argType) & TypeMasks::UINT_TYPES)
        op = OP_UMAX;
      else
        op = OP_IMAX;
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
      u16 texReg = args[0]; // Texture with 0x2000 marker
      const bool hasExplicitSampler =
          argCount >= 3 && (args[1] & 0xF000) == 0x3000;
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
                                                  static_cast<u16>(args[1] & 0x0FFFu));
      }

      // Texture samples always return float4
      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::GATHER:
    case Intrinsic::GATHER_OFFSET: {
      OpCode texOp = IntrinsicToOpcode(intrinsic);
      u16 texReg = args[0]; // Texture with 0x2000 marker
      const bool hasExplicitSampler =
          argCount >= 4 && (args[1] & 0xF000) == 0x3000;
      u16 coordReg = hasExplicitSampler ? args[2] : args[1];
      u16 componentReg = hasExplicitSampler ? args[3] : args[2];
      u16 offsetReg = 0x3FFF;
      if (intrinsic == Intrinsic::GATHER_OFFSET) {
        offsetReg = hasExplicitSampler ? args[4] : args[3];
      }

      builder.EmitInstruction(texOp, dest, texReg, coordReg, componentReg, offsetReg);
      if (hasExplicitSampler) {
        BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                  builder.currentInstruction - 1,
                                                  static_cast<u16>(args[1] & 0x0FFFu));
      }

      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::LOAD:
    case Intrinsic::LOAD_OFFSET: {
      OpCode texOp = IntrinsicToOpcode(intrinsic);
      u16 texReg = args[0];
      const bool hasExplicitSampler =
          argCount >= 4 && (args[1] & 0xF000) == 0x3000;
      u16 coordReg = hasExplicitSampler ? args[2] : args[1];
      u16 lodReg = hasExplicitSampler ? args[3] : args[2];
      u16 offsetReg = (intrinsic == Intrinsic::LOAD_OFFSET)
                          ? (hasExplicitSampler ? args[4] : args[3])
                          : 0x3FFF;
      builder.EmitInstruction(texOp, dest, texReg, coordReg, lodReg, offsetReg);
      if (hasExplicitSampler) {
        BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                  builder.currentInstruction - 1,
                                                  static_cast<u16>(args[1] & 0x0FFFu));
      }
      SetRegisterType(dest, CoreType::FLOAT4);
      return dest;
    }
    case Intrinsic::TEXTURE_SIZE: {
      u16 texReg = args[0];
      u16 lodReg = (argCount >= 2) ? args[1] : EmitConstantInt(0);
      builder.EmitInstruction(OP_TEX_SIZE, dest, texReg, lodReg);
      SetRegisterType(dest, CoreType::INT2);
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
    case Intrinsic::MODF_SPLIT: {
      static const char *fieldNames[] = {"fraction", "whole"};
      static const CoreType fieldTypes[] = {CoreType::FLOAT, CoreType::FLOAT};
      u32 structHash =
          RegisterBuiltinStructType("BwslModfResult", fieldNames,
                                    fieldTypes, 2);
      builder.EmitInstruction(OP_MODF_STRUCT, dest, args[0]);
      program.metadata[builder.currentInstruction - 1] = structHash;
      SetRegisterType(dest, CoreType::CUSTOM);
      program.registerStructTypes[dest] = structHash;
      return dest;
    }
    case Intrinsic::FREXP: {
      static const char *fieldNames[] = {"mantissa", "exponent"};
      static const CoreType fieldTypes[] = {CoreType::FLOAT, CoreType::INT};
      u32 structHash =
          RegisterBuiltinStructType("BwslFrexpResult", fieldNames,
                                    fieldTypes, 2);
      builder.EmitInstruction(OP_FREXP_STRUCT, dest, args[0]);
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
        resultType = CoreType::FLOAT;
        break;
      case Intrinsic::ANY:
      case Intrinsic::ALL:
        resultType = CoreType::BOOL;
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
      u16 inlinedResult = TryInlineFunction(call, args, argCount);
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

      // Check if this is a scalar type conversion (float(x), int(x), uint(x))
      bool isScalarConversion = (constructedType == CoreType::FLOAT ||
                                 constructedType == CoreType::INT ||
                                 constructedType == CoreType::UINT) &&
                                argCount == 1;

      if (isScalarConversion) {
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
      }
    } else {
      u32 callHash = call.name.nameHash;
      if (callHash == Utils::HashStr("float2x2") ||
          callHash == Utils::HashStr("float3x3") ||
          callHash == Utils::HashStr("float4x4")) {
        ReportError("Error: Matrix aliases float2x2/float3x3/float4x4 are not supported; use mat2, mat3, or mat4\n");
      }

      // Try to inline user function call
      u16 inlinedResult = TryInlineFunction(call, args, argCount);
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
          if (structTypeHash != 0) {
            auto it = structTypeMap.find(structTypeHash);
            u32 fieldCount = 0;
            if (it != structTypeMap.end()) {
              fieldCount = program.structTypes[it->second].fieldCount;
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
      }
    }
  }

  return dest;
}

inline u16 IRLowering::TryInlineFunction(const FunctionCallData &call, u16 *args, u32 argCount) {
  // Look up the function in the AST
  // Check if this is a module-qualified call (e.g.,
  // Globals::decompressPosition)
  NodeRef funcRef;
  u32 foundModuleIndex =
      0xFFFFFFFF; // Track which module we found the function in
  std::vector<OverloadTypeMask> argMasks;
  argMasks.reserve(argCount);
  for (u32 i = 0; i < argCount; i++) {
    CoreType argType = GetRegisterType(args[i]);
    u32 customHash = 0;
    if (argType == CoreType::CUSTOM) {
      customHash = program.registerStructTypes[args[i]];
    }
    argMasks.push_back(MakeOverloadMask(argType, customHash));
  }

  auto matchesSignature = [&](const FunctionDeclData &fn) -> bool {
    if (fn.parameters.count != argCount)
      return false;
    for (u32 i = 0; i < argCount; i++) {
      OverloadTypeMask paramMask = MakeOverloadMaskFromResolvedTypeHash(
          fn.parameters[i].second.nameHash);
      if (!OverloadMaskMatches(paramMask, argMasks[i])) {
        // Overload rejection is normal during resolution — caller
        // tries multiple candidates. Downstream "Function not found"
        // error covers the truly-unresolvable case.
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
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
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
            if (!matchesSignature(fn)) {
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
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
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
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
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
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
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
        if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
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
        fprintf(stderr,
                "Error: recursion is not supported — a function calls "
                "itself (directly or through another function). SPIR-V "
                "execution is stack-less; rewrite the algorithm "
                "iteratively.\n");
        recursionDiagnosed = true;
      }
      return 0xFFFF;
    }
  }

  // Check inline depth to prevent infinite recursion
  if (inlineDepth >= MAX_INLINE_DEPTH) {
    return 0xFFFF;
  }

  // Save current variable register mappings, struct types, and node register
  // cache Node register cache must be saved because we're re-lowering the
  // same AST nodes and each inlining needs fresh computations (not cached
  // results from previous inlining)
  auto savedVariableRegisters = variableRegisters;
  auto savedVariableStructTypes = variableStructTypes;
  auto savedNodeRegisters = nodeRegisters;

  // Bind parameters to argument registers
  u32 paramCount =
      func.parameters.count < argCount ? func.parameters.count : argCount;
  for (u32 i = 0; i < paramCount; i++) {
    const auto &param = func.parameters[i];
    u32 paramNameHash = param.first.nameHash; // Parameter name
    variableRegisters[paramNameHash] = args[i];

    // Also set the type for the parameter based on the type name
    // The second element of the pair is the type name (e.g., "uint",
    // "float3")
    u32 paramTypeHash = 0;
    CoreType paramType =
        ResolveCoreTypeFromHash(param.second.nameHash, &paramTypeHash);

    if (paramType != CoreType::INVALID && paramType != CoreType::VOID) {
      SetRegisterType(args[i], paramType);
      if ((paramType == CoreType::CUSTOM || paramType == CoreType::ENUM) &&
          paramTypeHash != 0) {
        u32 structHash = LookupOrRegisterStructType(paramTypeHash);

        if (structHash != 0) {
          program.registerStructTypes[args[i]] = structHash;
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
  u32 savedReturnCounter = inlineReturnCounter;
  u32 savedModuleIndex = inlineModuleIndex;
  inlineReturnReg = returnReg;
  inlineReturnFlagReg = AllocateRegister();
  SetRegisterType(inlineReturnFlagReg, CoreType::BOOL);
  builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg,
                          builder.EmitConstantBool(false));
  inlineReturnCounter = 0;
  inlineModuleIndex =
      foundModuleIndex; // Set module context for nested unqualified calls
  inlineDepth++;
  if (inlineStackDepth < MAX_INLINE_DEPTH) {
    inlineStackPacked[inlineStackDepth++] = funcRef.packed;
  }

  // Lower the function body
  if (func.body.Type() == ASTNodeType::BLOCK) {
    LowerBlock(func.body);
  } else {
    // Single expression body
    u16 exprResult = LowerExpression(func.body);
    builder.EmitInstruction(OP_STORE_REG, returnReg, exprResult);
  }

  // Restore state
  if (inlineStackDepth > 0) {
    inlineStackDepth--;
  }
  inlineDepth--;
  inlineReturnReg = savedReturnReg;
  inlineReturnFlagReg = savedReturnFlagReg;
  inlineReturnCounter = savedReturnCounter;
  inlineModuleIndex = savedModuleIndex;
  variableRegisters = savedVariableRegisters;
  variableStructTypes = savedVariableStructTypes;
  nodeRegisters = savedNodeRegisters;

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
  if (argCount > 16)
    return NodeRef::Null();

  for (u32 i = 0; i < argCount; i++) {
    argTypes[i] = GetRegisterType(args[i]);
    isConstrained[i] = false; // Will be set based on generic function
  }

  // Find a matching generic function (symbols is const, need to cast for
  // lookup)
  SymbolTableData *mutableSymbols = const_cast<SymbolTableData *>(symbols);
  GenericFunctionData *gfn = SymbolTable::FindMatchingGenericFunction(
      mutableSymbols, call.name.nameHash, argTypes, argCount);

  if (!gfn) {
    return NodeRef::Null(); // No matching generic function
  }

  // Build isConstrained array from the generic function's parameter info
  for (u32 i = 0; i < argCount && i < gfn->parameters.count; i++) {
    isConstrained[i] = gfn->parameters[i].isConstrained;
  }

  // Compute specialization hash
  u64 specHash = SpecializationRegistry::HashSpecialization(
      argTypes, isConstrained, argCount);

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
      gfn->name, argTypes, isConstrained, argCount);

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

  // IR format: s0 = texture (with 0x2000 marker for slot), s1 = coordinate
  if (texReg & 0x2000) {
    // texReg already has 0x2000 marker, pass it directly
    builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
  } else {
    // Bindless - texReg is a register containing texture handle
    builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
  }

  return dest;
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
