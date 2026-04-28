// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.

inline u16 IRLowering::EmitZeroStruct(u32 structTypeHash) {
  if (structTypeHash == 0) {
    return EmitZeroConstant(CoreType::FLOAT);
  }

  auto FindStructInfo = [&](u32 hash) -> const IRProgram::StructTypeInfo * {
    for (u32 i = 0; i < program.structTypeCount; i++) {
      if (program.structTypes[i].nameHash == hash) {
        return &program.structTypes[i];
      }
    }
    return nullptr;
  };

  // Find struct info, registering if needed (for nested custom types).
  const IRProgram::StructTypeInfo *structInfo =
      FindStructInfo(structTypeHash);
  if (!structInfo) {
    if (LookupOrRegisterStructType(structTypeHash) != 0) {
      structInfo = FindStructInfo(structTypeHash);
    }
  }
  if (!structInfo || structInfo->fieldCount == 0) {
    return EmitZeroConstant(CoreType::FLOAT);
  }

  u16 baseReg = AllocateRegister();
  SetRegisterType(baseReg, CoreType::CUSTOM);
  if (baseReg < MAX_REGISTERS) {
    program.registerStructTypes[baseReg] = structTypeHash;
  }
  AddUndefRegister(baseReg, CoreType::CUSTOM);

  u16 currentReg = baseReg;
  for (u32 f = 0; f < structInfo->fieldCount; f++) {
    CoreType fieldType = static_cast<CoreType>(
        program.structFieldTypes[structInfo->fieldOffset + f]);
    u32 fieldTypeHash = 0;
    if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
        program.structFieldTypeHashes) {
      fieldTypeHash =
          program.structFieldTypeHashes[structInfo->fieldOffset + f];
    }

    // Check if this field is an array - skip zero init for arrays
    // (they will remain undefined, which is valid in SPIR-V)
    u32 arraySize = 0;
    if (program.structFieldArraySizes) {
      arraySize = program.structFieldArraySizes[structInfo->fieldOffset + f];
    }
    if (arraySize > 0) {
      // Skip array fields - they remain as undef in the base struct
      continue;
    }

    u16 valueReg = 0xFFFF;
    if (fieldTypeHash != 0) {
      valueReg = EmitZeroStruct(fieldTypeHash);
    } else {
      valueReg = EmitZeroConstant(fieldType);
    }

    u16 nextReg = AllocateRegister();
    SetRegisterType(nextReg, CoreType::CUSTOM);
    if (nextReg < MAX_REGISTERS) {
      program.registerStructTypes[nextReg] = structTypeHash;
    }
    builder.EmitInstruction(OP_STRUCT_INSERT, nextReg, currentReg, f,
                            valueReg);
    program.metadata[program.instructionCount - 1] = structTypeHash;
    currentReg = nextReg;
  }

  return currentReg;
}

inline u16 IRLowering::EmitZeroConstant(CoreType type) {
  auto EmitZeroVector = [&](CoreType vecType, u16 zero,
                            u32 componentCount) -> u16 {
    u16 destReg = AllocateRegister();
    SetRegisterType(destReg, vecType);
    builder.EmitInstruction(OP_VEC_CONSTRUCT, destReg, zero, zero, zero,
                            zero);
    program.metadata[program.instructionCount - 1] = componentCount;
    return destReg;
  };

  switch (type) {
  case CoreType::FLOAT:
    return builder.EmitConstant(0.0f);
  case CoreType::BOOL:
    return builder.EmitConstantBool(false);
  case CoreType::FLOAT2:
  case CoreType::FLOAT3:
  case CoreType::FLOAT4: {
    u16 zero = builder.EmitConstant(0.0f);
    u32 componentCount = (type == CoreType::FLOAT2)   ? 2
                         : (type == CoreType::FLOAT3) ? 3
                                                      : 4;
    return EmitZeroVector(type, zero, componentCount);
  }
  case CoreType::INT:
    return EmitConstantInt(0);
  case CoreType::UINT:
    return EmitConstantUint(0);
  case CoreType::INT2:
  case CoreType::INT3:
  case CoreType::INT4: {
    u16 zero = EmitConstantInt(0);
    u32 componentCount = (type == CoreType::INT2)   ? 2
                         : (type == CoreType::INT3) ? 3
                                                    : 4;
    return EmitZeroVector(type, zero, componentCount);
  }
  case CoreType::UINT2:
  case CoreType::UINT3:
  case CoreType::UINT4: {
    u16 zero = EmitConstantUint(0);
    u32 componentCount = (type == CoreType::UINT2)   ? 2
                         : (type == CoreType::UINT3) ? 3
                                                     : 4;
    return EmitZeroVector(type, zero, componentCount);
  }
  case CoreType::BOOL2:
  case CoreType::BOOL3:
  case CoreType::BOOL4: {
    u16 zero = builder.EmitConstantBool(false);
    u32 componentCount = (type == CoreType::BOOL2)   ? 2
                         : (type == CoreType::BOOL3) ? 3
                                                     : 4;
    return EmitZeroVector(type, zero, componentCount);
  }
  case CoreType::MAT2:
  case CoreType::MAT3:
  case CoreType::MAT4: {
    u32 columnCount = (type == CoreType::MAT2)   ? 2
                      : (type == CoreType::MAT3) ? 3
                                                 : 4;
    CoreType columnType = (type == CoreType::MAT2)   ? CoreType::FLOAT2
                          : (type == CoreType::MAT3) ? CoreType::FLOAT3
                                                     : CoreType::FLOAT4;
    u32 componentCount = (columnType == CoreType::FLOAT2)   ? 2
                         : (columnType == CoreType::FLOAT3) ? 3
                                                            : 4;
    u16 zero = builder.EmitConstant(0.0f);
    u16 columns[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    for (u32 i = 0; i < columnCount; i++) {
      columns[i] = EmitZeroVector(columnType, zero, componentCount);
    }
    u16 destReg = AllocateRegister();
    SetRegisterType(destReg, type);
    builder.EmitInstruction(OP_MAT_CONSTRUCT, destReg, columns[0], columns[1],
                            columns[2], columns[3]);
    program.metadata[program.instructionCount - 1] = columnCount;
    return destReg;
  }
  default:
    return builder.EmitConstant(0.0f);
  }
}

inline u32 IRLowering::LookupOrRegisterStructType(u32 typeNameHash) {
  // Check if already registered with this hash
  auto it = structTypeMap.find(typeNameHash);
  if (it != structTypeMap.end()) {
    // Return the canonical hash stored in structTypes, not the lookup hash
    u32 structIdx = it->second;
    return program.structTypes[structIdx].nameHash;
  }

  // Look up in symbol table
  Symbol *typeSym = SymbolTable::LookupByHash(
      const_cast<SymbolTableData *>(symbols), typeNameHash);
  if (!typeSym || typeSym->kind != SymbolKind::CUSTOM_TYPE) {
    // Check if it's an enum type (ENUM for module enums, ENUM_SYMBOL for
    // pipeline-local enums)
    if (typeSym && (typeSym->kind == SymbolKind::ENUM ||
                    typeSym->kind == SymbolKind::ENUM_SYMBOL)) {
      const EnumData &enumData = symbols->enums[typeSym->index];
      if (enumData.flags & EnumData::IS_SUM_TYPE) {
        return RegisterEnumAsStructType(typeNameHash, typeSym->index);
      }
      return 0;
    }

    // Try looking up via global custom type registry
    StructData *structData = g_customTypes.LookupType(typeNameHash);
    if (structData) {
      return RegisterStructTypeFromData(typeNameHash, structData);
    }
    return 0;
  }
  // Get the struct data - use its name hash as canonical (unqualified name
  // from struct definition) This is important for module-qualified types like
  // PBR::PBRMaterial where:
  // - typeSym->name.nameHash is the qualified hash "PBR::PBRMaterial"
  // - structData.name.nameHash is the unqualified hash "PBRMaterial"
  const StructData &structData = symbols->structs[typeSym->index];
  u32 canonicalHash = structData.name.nameHash;

  // Check if already registered with the canonical hash (may differ from
  // lookup hash)
  auto canonIt = structTypeMap.find(canonicalHash);
  if (canonIt != structTypeMap.end()) {
    // Also register the lookup hash as an alias to the same struct
    if (typeNameHash != canonicalHash) {
      structTypeMap[typeNameHash] = canonIt->second;
    }
    return canonicalHash;
  }

  // Register with canonical hash
  u32 result = RegisterStructTypeFromSymbol(canonicalHash, structData);

  // Also register the lookup hash and symbol name hash as aliases if
  // different
  if (result != 0) {
    u32 structIdx = structTypeMap[canonicalHash];
    if (typeNameHash != canonicalHash) {
      structTypeMap[typeNameHash] = structIdx;
    }
    if (typeSym->name.nameHash != canonicalHash) {
      structTypeMap[typeSym->name.nameHash] = structIdx;
    }
  }

  return result;
}

inline u32 IRLowering::RegisterStructTypeFromSymbol(u32 typeNameHash,
                                 const StructData &structData) {
  if (program.structTypeCount >= program.structTypeCapacity) {
    return 0; // Out of capacity
  }

  u32 structIdx = program.structTypeCount++;
  structTypeMap[typeNameHash] = structIdx;

  IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
  info.nameHash = typeNameHash;
  info.fieldCount = structData.fields.count;
  info.fieldOffset = 0; // Will be set below

  // Calculate field offset in flattened arrays
  u32 fieldOffset = 0;
  for (u32 i = 0; i < structIdx; i++) {
    fieldOffset += program.structTypes[i].fieldCount;
  }
  info.fieldOffset = fieldOffset;

  // Calculate total size and copy field info (std140 layout)
  u32 currentOffset = 0;
  for (u32 i = 0; i < structData.fields.count; i++) {
    if (fieldOffset + i >= program.structFieldCapacity)
      break;

    const StructData::Field &field = structData.fields[i];
    program.structFieldTypes[fieldOffset + i] =
        static_cast<u16>(field.type.coreType);
    program.structFieldNameHashes[fieldOffset + i] = field.name.nameHash;
    program.structFieldTypeHashes[fieldOffset + i] =
        (field.type.coreType == CoreType::CUSTOM) ? field.type.customTypeHash
                                                  : 0;
    program.structFieldArraySizes[fieldOffset + i] = field.arraySize;

    // std140 alignment rules - for arrays, account for array length
    u32 fieldSize = GetTypeSize(field.type.coreType);
    u32 alignment = GetTypeAlignment(field.type.coreType);

    // For CUSTOM types (nested structs), ensure nested struct is registered
    // and get its size
    if (field.type.coreType == CoreType::CUSTOM &&
        field.type.customTypeHash != 0) {
      // First, ensure the nested struct is registered (recursively)
      auto it = structTypeMap.find(field.type.customTypeHash);
      if (it == structTypeMap.end()) {
        // Nested struct not registered yet - register it now
        LookupOrRegisterStructType(field.type.customTypeHash);
        it = structTypeMap.find(field.type.customTypeHash);
      }

      if (it != structTypeMap.end()) {
        u32 nestedIdx = it->second;
        fieldSize = program.structTypes[nestedIdx].totalSize;
        // Struct alignment is max of its member alignments, minimum 16 for
        // std140
        alignment = 16;
      }
    }

    currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
    program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
    if (field.arraySize > 0) {
      // std430 layout: scalars and vec2 use natural stride, vec3+ rounds to
      // 16
      u32 arrayStride;
      if (fieldSize <= 8) {
        arrayStride = fieldSize; // Natural size for scalars (4) and vec2 (8)
      } else {
        arrayStride =
            (fieldSize + 15) & ~15; // Round to 16 for vec3, vec4, mat
      }
      currentOffset += arrayStride * field.arraySize;
    } else {
      currentOffset += fieldSize;
    }
  }

  info.totalSize = currentOffset;
  return typeNameHash;
}

inline u32 IRLowering::RegisterStructTypeFromData(u32 typeNameHash, StructData *structData) {
  if (!structData || program.structTypeCount >= program.structTypeCapacity) {
    return 0;
  }

  u32 structIdx = program.structTypeCount++;
  structTypeMap[typeNameHash] = structIdx;

  IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
  info.nameHash = typeNameHash;
  info.fieldCount = structData->fields.count;

  // Calculate field offset in flattened arrays
  u32 fieldOffset = 0;
  for (u32 i = 0; i < structIdx; i++) {
    fieldOffset += program.structTypes[i].fieldCount;
  }
  info.fieldOffset = fieldOffset;

  // Calculate total size and copy field info
  u32 currentOffset = 0;
  for (u32 i = 0; i < structData->fields.count; i++) {
    if (fieldOffset + i >= program.structFieldCapacity)
      break;

    const StructData::Field &field = structData->fields[i];
    program.structFieldTypes[fieldOffset + i] =
        static_cast<u16>(field.type.coreType);
    program.structFieldNameHashes[fieldOffset + i] = field.name.nameHash;
    program.structFieldTypeHashes[fieldOffset + i] =
        (field.type.coreType == CoreType::CUSTOM) ? field.type.customTypeHash
                                                  : 0;
    program.structFieldArraySizes[fieldOffset + i] = field.arraySize;

    u32 fieldSize = GetTypeSize(field.type.coreType);
    u32 alignment = GetTypeAlignment(field.type.coreType);

    // For CUSTOM types (nested structs), ensure nested struct is registered
    // and get its size
    if (field.type.coreType == CoreType::CUSTOM &&
        field.type.customTypeHash != 0) {
      // First, ensure the nested struct is registered (recursively)
      auto it = structTypeMap.find(field.type.customTypeHash);
      if (it == structTypeMap.end()) {
        // Nested struct not registered yet - register it now
        LookupOrRegisterStructType(field.type.customTypeHash);
        it = structTypeMap.find(field.type.customTypeHash);
      }

      if (it != structTypeMap.end()) {
        u32 nestedIdx = it->second;
        fieldSize = program.structTypes[nestedIdx].totalSize;
        // Struct alignment is max of its member alignments, minimum 16 for
        // std140
        alignment = 16;
      }
    }

    currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
    program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
    if (field.arraySize > 0) {
      // std430 layout: scalars and vec2 use natural stride, vec3+ rounds to
      // 16
      u32 arrayStride;
      if (fieldSize <= 8) {
        arrayStride = fieldSize; // Natural size for scalars (4) and vec2 (8)
      } else {
        arrayStride =
            (fieldSize + 15) & ~15; // Round to 16 for vec3, vec4, mat
      }
      currentOffset += arrayStride * field.arraySize;
    } else {
      currentOffset += fieldSize;
    }
  }

  info.totalSize = currentOffset;
  return typeNameHash;
}

inline u32 IRLowering::RegisterBuiltinStructType(const char *typeName,
                              const char *const *fieldNames,
                              const CoreType *fieldTypes,
                              u32 fieldCount) {
  if (!typeName || !fieldNames || !fieldTypes || fieldCount == 0 ||
      program.structTypeCount >= program.structTypeCapacity) {
    return 0;
  }

  u32 typeNameHash = Utils::HashStr(typeName);
  auto existing = structTypeMap.find(typeNameHash);
  if (existing != structTypeMap.end()) {
    return typeNameHash;
  }

  u32 structIdx = program.structTypeCount++;
  structTypeMap[typeNameHash] = structIdx;
  ReverseLookup::Register(typeNameHash, typeName);

  IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
  info.nameHash = typeNameHash;
  info.fieldCount = fieldCount;

  u32 fieldOffset = 0;
  for (u32 i = 0; i < structIdx; i++) {
    fieldOffset += program.structTypes[i].fieldCount;
  }
  info.fieldOffset = fieldOffset;

  if (fieldOffset + fieldCount > program.structFieldCapacity) {
    program.structTypeCount--;
    structTypeMap.erase(typeNameHash);
    return 0;
  }

  u32 currentOffset = 0;
  for (u32 i = 0; i < fieldCount; i++) {
    u32 fieldNameHash = Utils::HashStr(fieldNames[i]);
    ReverseLookup::Register(fieldNameHash, fieldNames[i]);

    program.structFieldTypes[fieldOffset + i] =
        static_cast<u16>(fieldTypes[i]);
    program.structFieldNameHashes[fieldOffset + i] = fieldNameHash;
    program.structFieldTypeHashes[fieldOffset + i] = 0;
    program.structFieldArraySizes[fieldOffset + i] = 0;

    u32 alignment = GetTypeAlignment(fieldTypes[i]);
    currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
    program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
    currentOffset += GetTypeSize(fieldTypes[i]);
  }

  info.totalSize = currentOffset;
  return typeNameHash;
}

inline bool IRLowering::IsSumEnumTypeHash(u32 typeHash) {
  const EnumData *enumData =
      SymbolTable::ResolveEnumDataByHash(symbols, typeHash);
  return enumData && (enumData->flags & EnumData::IS_SUM_TYPE);
}

inline StructData *IRLowering::LookupStructDataByHash(u32 typeHash) {
  if (!symbols || typeHash == 0) {
    return nullptr;
  }

  Symbol *sym = SymbolTable::LookupByHash(
      const_cast<SymbolTableData *>(symbols), typeHash);
  if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
    return const_cast<StructData *>(&symbols->structs[sym->index]);
  }

  return g_customTypes.LookupType(typeHash);
}

inline u32 IRLowering::ScalarComponentCount(CoreType type) {
  return CoreTypeScalarComponentCount(type);
}

inline void IRLowering::AppendEnumPayloadLayout(CoreType type, u32 typeHash,
                             CoreType *fieldTypes, u32 *fieldTypeHashes,
                             u32 &fieldCount, u32 maxFields) {
  if (fieldCount >= maxFields) {
    return;
  }

  if (type == CoreType::CUSTOM && typeHash != 0) {
    if (IsSumEnumTypeHash(typeHash)) {
      fieldTypes[fieldCount] = CoreType::CUSTOM;
      fieldTypeHashes[fieldCount] = typeHash;
      fieldCount++;
      return;
    }

    if (StructData *structData = LookupStructDataByHash(typeHash)) {
      for (u32 i = 0; i < structData->fields.count && fieldCount < maxFields;
           i++) {
        const StructData::Field &field = structData->fields[i];
        u32 fieldHash = (field.type.coreType == CoreType::CUSTOM)
                            ? field.type.customTypeHash
                            : 0;
        AppendEnumPayloadLayout(field.type.coreType, fieldHash, fieldTypes,
                                fieldTypeHashes, fieldCount, maxFields);
      }
      return;
    }

    fieldTypes[fieldCount] = CoreType::CUSTOM;
    fieldTypeHashes[fieldCount] = typeHash;
    fieldCount++;
    return;
  }

  CoreType scalarType = GetScalarComponentType(type);
  u32 componentCount = ScalarComponentCount(type);
  for (u32 c = 0; c < componentCount && fieldCount < maxFields; c++) {
    fieldTypes[fieldCount] = scalarType;
    fieldTypeHashes[fieldCount] = 0;
    fieldCount++;
  }
}

inline u32 IRLowering::GetEnumPayloadFieldCount(CoreType type, u32 typeHash) {
  CoreType fieldTypes[64];
  u32 fieldTypeHashes[64];
  u32 fieldCount = 0;
  AppendEnumPayloadLayout(type, typeHash, fieldTypes, fieldTypeHashes,
                          fieldCount, 64);
  return fieldCount;
}

inline u32 IRLowering::RegisterEnumAsStructType(u32 enumNameHash, u32 enumIndex) {
  if (!symbols || enumIndex >= symbols->enums.count) {
    return 0;
  }
  if (program.structTypeCount >= program.structTypeCapacity) {
    return 0;
  }

  const EnumData &enumData = symbols->enums[enumIndex];
  static constexpr u32 MAX_ENUM_PAYLOAD_FIELDS = 64;
  CoreType payloadFieldTypes[MAX_ENUM_PAYLOAD_FIELDS];
  u32 payloadFieldTypeHashes[MAX_ENUM_PAYLOAD_FIELDS];
  bool payloadFieldSet[MAX_ENUM_PAYLOAD_FIELDS];
  for (u32 i = 0; i < MAX_ENUM_PAYLOAD_FIELDS; i++) {
    payloadFieldTypes[i] = CoreType::FLOAT;
    payloadFieldTypeHashes[i] = 0;
    payloadFieldSet[i] = false;
  }

  u32 maxPayloadFieldCount = 0;
  for (u32 v = 0; v < enumData.variants.count; v++) {
    const EnumData::Variant &variant = enumData.variants[v];
    CoreType variantFieldTypes[MAX_ENUM_PAYLOAD_FIELDS];
    u32 variantFieldTypeHashes[MAX_ENUM_PAYLOAD_FIELDS];
    u32 variantFieldCount = 0;

    for (u32 t = 0; t < variant.associatedTypes.count; t++) {
      CoreType assocType = variant.associatedTypes[t];
      u32 assocHash = 0;
      if (t < variant.associatedTypeHashes.count) {
        assocHash = variant.associatedTypeHashes[t];
      }
      AppendEnumPayloadLayout(assocType, assocHash, variantFieldTypes,
                              variantFieldTypeHashes, variantFieldCount,
                              MAX_ENUM_PAYLOAD_FIELDS);
    }

    for (u32 i = 0; i < variantFieldCount; i++) {
      if (!payloadFieldSet[i]) {
        payloadFieldTypes[i] = variantFieldTypes[i];
        payloadFieldTypeHashes[i] = variantFieldTypeHashes[i];
        payloadFieldSet[i] = true;
      } else if (payloadFieldTypes[i] != variantFieldTypes[i] ||
                 payloadFieldTypeHashes[i] != variantFieldTypeHashes[i]) {
        // Existing enum storage used float payload slots for heterogeneous
        // variants. Keep that fallback for conflicts while preserving
        // precise types when all variants agree at a payload position.
        payloadFieldTypes[i] = CoreType::FLOAT;
        payloadFieldTypeHashes[i] = 0;
      }
    }

    if (variantFieldCount > maxPayloadFieldCount) {
      maxPayloadFieldCount = variantFieldCount;
    }
  }

  u32 totalFields = 1 + maxPayloadFieldCount; // tag + payload fields

  u32 structIdx = program.structTypeCount++;
  structTypeMap[enumNameHash] = structIdx;

  IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
  info.nameHash = enumNameHash;
  info.fieldCount = totalFields;

  // Calculate field offset in flattened arrays
  u32 fieldOffset = 0;
  for (u32 i = 0; i < structIdx; i++) {
    fieldOffset += program.structTypes[i].fieldCount;
  }
  info.fieldOffset = fieldOffset;

  if (fieldOffset + totalFields > program.structFieldCapacity) {
    program.structTypeCount--; // Rollback
    return 0;
  }

  // Field 0: tag (int)
  u32 currentOffset = 0;
  program.structFieldTypes[fieldOffset] = static_cast<u16>(CoreType::INT);
  program.structFieldNameHashes[fieldOffset] = Utils::HashStr("tag");
  program.structFieldTypeHashes[fieldOffset] = 0;
  program.structFieldArraySizes[fieldOffset] = 0;
  program.structFieldByteOffsets[fieldOffset] = currentOffset;
  currentOffset += 4; // int is 4 bytes

  // Fields 1..N: payload storage
  for (u32 i = 0; i < maxPayloadFieldCount; i++) {
    u32 fIdx = fieldOffset + 1 + i;
    CoreType fieldType = payloadFieldTypes[i];
    u32 fieldTypeHash = payloadFieldTypeHashes[i];

    program.structFieldTypes[fIdx] = static_cast<u16>(fieldType);
    // Generate field name like "f0", "f1", etc.
    char fieldName[16];
    snprintf(fieldName, sizeof(fieldName), "f%u", i);
    program.structFieldNameHashes[fIdx] = Utils::HashStr(fieldName);
    program.structFieldTypeHashes[fIdx] =
        (fieldType == CoreType::CUSTOM) ? fieldTypeHash : 0;
    program.structFieldArraySizes[fIdx] = 0;

    u32 fieldSize = GetTypeSize(fieldType);
    u32 alignment = GetTypeAlignment(fieldType);
    if (fieldType == CoreType::CUSTOM && fieldTypeHash != 0) {
      u32 nestedHash = LookupOrRegisterStructType(fieldTypeHash);
      auto nestedIt =
          structTypeMap.find(nestedHash != 0 ? nestedHash : fieldTypeHash);
      if (nestedIt != structTypeMap.end()) {
        fieldSize = program.structTypes[nestedIt->second].totalSize;
        alignment = 16;
      }
    }

    currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
    program.structFieldByteOffsets[fIdx] = currentOffset;
    currentOffset += fieldSize;
  }

  info.totalSize = currentOffset;
  return enumNameHash;
}

inline CoreType IRLowering::InferResourceType(u32 nameHash) {
  // Common matrix resource name hashes
  // These are computed from Utils::HashStr at compile time for known names
  static const u32 HASH_modelMatrix = Utils::HashStr("modelMatrix");
  static const u32 HASH_viewMatrix = Utils::HashStr("viewMatrix");
  static const u32 HASH_projMatrix = Utils::HashStr("projMatrix");
  static const u32 HASH_projectionMatrix = Utils::HashStr("projectionMatrix");
  static const u32 HASH_viewProjectionMatrix =
      Utils::HashStr("viewProjectionMatrix");
  static const u32 HASH_viewProjMatrix = Utils::HashStr("viewProjMatrix");
  static const u32 HASH_modelViewMatrix = Utils::HashStr("modelViewMatrix");
  static const u32 HASH_modelViewProjectionMatrix =
      Utils::HashStr("modelViewProjectionMatrix");
  static const u32 HASH_mvpMatrix = Utils::HashStr("mvpMatrix");
  static const u32 HASH_normalMatrix = Utils::HashStr("normalMatrix");
  static const u32 HASH_inverseViewMatrix =
      Utils::HashStr("inverseViewMatrix");
  static const u32 HASH_inverseProjMatrix =
      Utils::HashStr("inverseProjMatrix");
  static const u32 HASH_lightViewProjMatrix =
      Utils::HashStr("lightViewProjMatrix");
  static const u32 HASH_previousModelMatrix =
      Utils::HashStr("previousModelMatrix");
  static const u32 HASH_previousViewProjMatrix =
      Utils::HashStr("previousViewProjMatrix");
  static const u32 HASH_boneMatrices = Utils::HashStr("boneMatrices");

  // Check for known matrix names
  if (nameHash == HASH_modelMatrix || nameHash == HASH_viewMatrix ||
      nameHash == HASH_projMatrix || nameHash == HASH_projectionMatrix ||
      nameHash == HASH_viewProjectionMatrix ||
      nameHash == HASH_viewProjMatrix || nameHash == HASH_modelViewMatrix ||
      nameHash == HASH_modelViewProjectionMatrix ||
      nameHash == HASH_mvpMatrix || nameHash == HASH_normalMatrix ||
      nameHash == HASH_inverseViewMatrix ||
      nameHash == HASH_inverseProjMatrix ||
      nameHash == HASH_lightViewProjMatrix ||
      nameHash == HASH_previousModelMatrix ||
      nameHash == HASH_previousViewProjMatrix ||
      nameHash == HASH_boneMatrices) {
    return CoreType::MAT4;
  }

  // Common vector names
  static const u32 HASH_cameraPosition = Utils::HashStr("cameraPosition");
  static const u32 HASH_lightPosition = Utils::HashStr("lightPosition");
  static const u32 HASH_lightDirection = Utils::HashStr("lightDirection");
  static const u32 HASH_lightColor = Utils::HashStr("lightColor");
  static const u32 HASH_ambientColor = Utils::HashStr("ambientColor");

  if (nameHash == HASH_cameraPosition || nameHash == HASH_lightPosition ||
      nameHash == HASH_lightDirection) {
    return CoreType::FLOAT3;
  }
  if (nameHash == HASH_lightColor || nameHash == HASH_ambientColor) {
    return CoreType::FLOAT4;
  }

  // Default to float4 for unknown buffer resources
  // The proper fix is to infer type from usage context or require explicit
  // type declarations in the render config
  return CoreType::FLOAT4;
}

inline u32 IRLowering::GetTypeSize(CoreType type) {
  return CoreTypeStorageSize(type);
}

inline u32 IRLowering::GetTypeAlignment(CoreType type) {
  return CoreTypeStd140Alignment(type);
}

inline u32 IRLowering::GetCoreTypeNameHash(CoreType type) {
  return SymbolTable::GetCoreTypeNameHash(type);
}

inline u16 IRLowering::AllocateRegister() {
  // Cap at MAX_REGISTERS - 1. Pathological inputs can exhaust u16 register
  // space; once past MAX_REGISTERS, registerStorageInfo / registerTypes
  // indexing OOBs. Returning the same sentinel repeatedly produces bad
  // SPIR-V (SPIR-V validation will reject it) but avoids the crash.
  if (builder.nextRegister >= MAX_REGISTERS - 1) {
    return MAX_REGISTERS - 1;
  }
  u16 reg = builder.nextRegister++;
  if (reg >= program.registerCount) {
    program.registerCount = reg + 1;
  }
  return reg;
}

inline u16 IRLowering::EmitConstantInt(u32 value) {
  for (u32 i = 0; i < program.intCount; i++) {
    if (program.intConstants[i] == value) {
      return 0x4000 | i;
    }
  }
  u32 slot = program.intCount++;
  program.intConstants[slot] = value;
  return 0x4000 | slot;
}

inline u16 IRLowering::EmitConstantUint(u32 value) {
  for (u32 i = 0; i < program.uintCount; i++) {
    if (program.uintConstants[i] == value) {
      return 0x2000 | i;
    }
  }
  u32 slot = program.uintCount++;
  program.uintConstants[slot] = value;
  return 0x2000 | slot;
}

inline u16 IRLowering::GetOrAllocateVariable(u32 nameHash) {
  auto it = variableRegisters.find(nameHash);
  if (it != variableRegisters.end()) {
    return it->second;
  }
  u16 reg = AllocateRegister();
  variableRegisters[nameHash] = reg;
  return reg;
}

inline CoreType IRLowering::GetRegisterType(u16 reg) {
  // Constants have type encoded in high bits
  // Check order matters: bool (0xC000) must be checked before float (0x8000)
  if ((reg & 0xC000) == 0xC000)
    return CoreType::BOOL; // Bool constant (0xC000 prefix)
  if (reg & 0x8000)
    return CoreType::FLOAT; // Float constant (0x8000 prefix)
  if (reg & 0x4000)
    return CoreType::INT; // Int constant (0x4000 prefix)
  if (reg & 0x2000)
    return CoreType::UINT; // Uint constant (0x2000 prefix)

  if (reg < program.registerCount) {
    return static_cast<CoreType>(program.registerTypes[reg]);
  }
  return CoreType::FLOAT; // Default
}

inline void IRLowering::SetRegisterType(u16 reg, CoreType type) {
  if (reg < MAX_REGISTERS) {
    program.registerTypes[reg] = static_cast<u16>(type);
  }
}

inline void IRLowering::AddUndefRegister(u16 reg, CoreType type) {
  if (program.undefRegCount >= program.undefRegCapacity) {
    u32 newCapacity = program.undefRegCapacity * 2;
    u16 *newRegs = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
    u16 *newTypes = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
    memcpy(newRegs, program.undefRegs,
           program.undefRegCapacity * sizeof(u16));
    memcpy(newTypes, program.undefRegTypes,
           program.undefRegCapacity * sizeof(u16));
    program.undefRegs = newRegs;
    program.undefRegTypes = newTypes;
    program.undefRegCapacity = newCapacity;
  }
  program.undefRegs[program.undefRegCount] = reg;
  program.undefRegTypes[program.undefRegCount] = static_cast<u16>(type);
  program.undefRegCount++;
}

inline CoreType IRLowering::GetScalarComponentType(CoreType type) {
  switch (type) {
  case CoreType::FLOAT2:
  case CoreType::FLOAT3:
  case CoreType::FLOAT4:
    return CoreType::FLOAT;
  case CoreType::INT2:
  case CoreType::INT3:
  case CoreType::INT4:
    return CoreType::INT;
  case CoreType::UINT2:
  case CoreType::UINT3:
  case CoreType::UINT4:
    return CoreType::UINT;
  case CoreType::BOOL2:
  case CoreType::BOOL3:
  case CoreType::BOOL4:
    return CoreType::BOOL;
  default:
    return type; // Already scalar or not a vector
  }
}

inline CoreType IRLowering::GetVectorType(CoreType scalarType, int componentCount) {
  switch (scalarType) {
  case CoreType::FLOAT:
    if (componentCount == 2)
      return CoreType::FLOAT2;
    if (componentCount == 3)
      return CoreType::FLOAT3;
    if (componentCount == 4)
      return CoreType::FLOAT4;
    break;
  case CoreType::INT:
    if (componentCount == 2)
      return CoreType::INT2;
    if (componentCount == 3)
      return CoreType::INT3;
    if (componentCount == 4)
      return CoreType::INT4;
    break;
  case CoreType::UINT:
    if (componentCount == 2)
      return CoreType::UINT2;
    if (componentCount == 3)
      return CoreType::UINT3;
    if (componentCount == 4)
      return CoreType::UINT4;
    break;
  case CoreType::BOOL:
    if (componentCount == 2)
      return CoreType::BOOL2;
    if (componentCount == 3)
      return CoreType::BOOL3;
    if (componentCount == 4)
      return CoreType::BOOL4;
    break;
  default:
    break;
  }
  return scalarType; // Fallback to scalar
}

inline CoreType IRLowering::InferExpressionType(NodeRef ref) {
  if (ref.IsNull())
    return CoreType::INT; // Default

  switch (ref.Type()) {
  case ASTNodeType::LITERAL: {
    const LiteralData &lit = ast->GetLiteral(ref);
    // Convert LiteralValue::Type to CoreType
    switch (lit.value.type) {
    case LiteralValue::FLOAT:
      return CoreType::FLOAT;
    case LiteralValue::INT:
      return CoreType::INT;
    case LiteralValue::UINT:
      return CoreType::UINT;
    case LiteralValue::BOOL:
      return CoreType::BOOL;
    default:
      return CoreType::INT;
    }
  }
  case ASTNodeType::IDENTIFIER: {
    const IdentifierData &ident = ast->GetIdentifier(ref);
    // Check if it's a variable we've already allocated
    auto it = variableRegisters.find(ident.name.nameHash);
    if (it != variableRegisters.end()) {
      return GetRegisterType(it->second);
    }
    // Check function parameters (during inlining)
    // For now, assume INT as default for identifiers
    return CoreType::INT;
  }
  case ASTNodeType::BINARY_OP: {
    const BinaryOpData &binOp = ast->GetBinaryOp(ref);
    // For arithmetic ops, type is determined by operands
    CoreType leftType = InferExpressionType(binOp.left);
    return leftType; // Simplified: just use left operand type
  }
  case ASTNodeType::FUNCTION_CALL: {
    // Function return type - check if it's a known function
    // For now, default to FLOAT for function calls
    return CoreType::FLOAT;
  }
  default:
    return CoreType::INT; // Default fallback
  }
}

inline bool IRLowering::IsExpressionUnsigned(NodeRef ref) {
  CoreType type = InferExpressionType(ref);
  return (type == CoreType::UINT || type == CoreType::UINT2 ||
          type == CoreType::UINT3 || type == CoreType::UINT4);
}

inline u32 IRLowering::GetVectorDimension(CoreType type) {
  return CoreTypeScalarComponentCount(type);
}

inline CoreType IRLowering::GetVectorTypeWithDimension(CoreType baseType, u32 dim) {
  // Determine the base element type
  bool isFloat =
      (baseType == CoreType::FLOAT || baseType == CoreType::FLOAT2 ||
       baseType == CoreType::FLOAT3 || baseType == CoreType::FLOAT4);
  bool isInt = (baseType == CoreType::INT || baseType == CoreType::INT2 ||
                baseType == CoreType::INT3 || baseType == CoreType::INT4);
  bool isUint = (baseType == CoreType::UINT || baseType == CoreType::UINT2 ||
                 baseType == CoreType::UINT3 || baseType == CoreType::UINT4);

  if (isFloat) {
    switch (dim) {
    case 1:
      return CoreType::FLOAT;
    case 2:
      return CoreType::FLOAT2;
    case 3:
      return CoreType::FLOAT3;
    case 4:
      return CoreType::FLOAT4;
    }
  } else if (isInt) {
    switch (dim) {
    case 1:
      return CoreType::INT;
    case 2:
      return CoreType::INT2;
    case 3:
      return CoreType::INT3;
    case 4:
      return CoreType::INT4;
    }
  } else if (isUint) {
    switch (dim) {
    case 1:
      return CoreType::UINT;
    case 2:
      return CoreType::UINT2;
    case 3:
      return CoreType::UINT3;
    case 4:
      return CoreType::UINT4;
    }
  }
  return baseType; // Return as-is if unknown
}

inline CoreType IRLowering::LookupCoreType(u32 typeHash) {
  // Use TypeHashes from bwsl_types.h
  for (u32 i = 0; i < TypeHashes::HASH_TABLE_SIZE; i++) {
    if (TypeHashes::HASH_TABLE[i].hash == typeHash) {
      return TypeHashes::HASH_TABLE[i].info.coreType;
    }
  }
  return CoreType::INVALID;
}

inline CoreType IRLowering::ResolveCoreTypeFromHash(u32 typeHash, u32 *outCustomHash) {
  if (outCustomHash) {
    *outCustomHash = 0;
  }

  CoreType coreType = LookupCoreType(typeHash);
  // GENERIC_T/U/V collide with single-letter user struct names (`V`,
  // `T`, `U` — common in shader code for vertex, texture, etc.). Prefer
  // a user-defined struct/enum over the generic placeholder if one
  // exists. GENERIC_* is only meaningful inside generic-fn signatures,
  // which have their own type-resolution path.
  if (coreType == CoreType::GENERIC_T || coreType == CoreType::GENERIC_U ||
      coreType == CoreType::GENERIC_V) {
    Symbol *userSym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), typeHash);
    if (userSym && (userSym->kind == SymbolKind::CUSTOM_TYPE ||
                    userSym->kind == SymbolKind::ENUM ||
                    userSym->kind == SymbolKind::ENUM_SYMBOL)) {
      coreType = CoreType::INVALID; // fall through to user-symbol path
    }
  }
  if (coreType != CoreType::INVALID && coreType != CoreType::VOID) {
    return coreType;
  }

  Symbol *sym = SymbolTable::LookupByHash(
      const_cast<SymbolTableData *>(symbols), typeHash);
  if (sym && (sym->kind == SymbolKind::ENUM ||
              sym->kind == SymbolKind::ENUM_SYMBOL)) {
    const EnumData &enumData = symbols->enums[sym->index];
    if (enumData.flags & EnumData::IS_SUM_TYPE) {
      if (outCustomHash) {
        *outCustomHash = enumData.name.nameHash;
      }
      return CoreType::CUSTOM;
    }

    CoreType baseType = enumData.underlyingType;
    if (baseType == CoreType::INVALID) {
      baseType = CoreType::INT;
    }
    return baseType;
  }

  if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
    if (outCustomHash) {
      // Use the struct's unqualified name hash for consistency
      const StructData &structData = symbols->structs[sym->index];
      *outCustomHash = structData.name.nameHash;
    }
    return CoreType::CUSTOM;
  }

  // Fall back to global custom type registry
  // This handles cases where the type is known by unqualified name (e.g.,
  // "PBRMaterial") but registered in symbol table with qualified name (e.g.,
  // "PBR::PBRMaterial")
  StructData *structData = g_customTypes.LookupType(typeHash);
  if (structData) {
    if (outCustomHash) {
      *outCustomHash = structData->name.nameHash;
    }
    return CoreType::CUSTOM;
  }

  return CoreType::INVALID;
}

inline OverloadTypeMask IRLowering::MakeOverloadMaskFromResolvedTypeHash(u32 typeHash) {
  u32 customHash = 0;
  CoreType coreType = ResolveCoreTypeFromHash(typeHash, &customHash);
  if (coreType == CoreType::CUSTOM && customHash != 0) {
    return MakeOverloadMask(coreType, customHash);
  }
  if (coreType != CoreType::INVALID && coreType != CoreType::VOID) {
    return MakeOverloadMask(coreType);
  }
  return MakeOverloadMaskFromTypeHash(typeHash);
}

