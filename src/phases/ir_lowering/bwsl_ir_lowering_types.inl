// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.
#pragma once
#include "bwsl_ir_lowering.h"

namespace BWSL::IR {

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
  for (u16 f = 0; f < structInfo->fieldCount; f++) {
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
  u32 resolvedAliasHash = SymbolTable::ResolveTypeAliasHashInScope(
      symbols, typeNameHash, AliasOwnerKind(), AliasOwnerModuleIndex());
  if (resolvedAliasHash != typeNameHash) {
    u32 result = LookupOrRegisterStructType(resolvedAliasHash);
    if (result != 0) {
      auto resolvedIt = structTypeMap.find(resolvedAliasHash);
      if (resolvedIt != structTypeMap.end()) {
        structTypeMap[typeNameHash] = resolvedIt->second;
      } else {
        auto canonicalIt = structTypeMap.find(result);
        if (canonicalIt != structTypeMap.end()) {
          structTypeMap[typeNameHash] = canonicalIt->second;
        }
      }
    }
    return result;
  }

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

    const EnumData *resolvedEnum =
        SymbolTable::ResolveEnumDataByHash(symbols, typeNameHash);
    if (resolvedEnum && (resolvedEnum->flags & EnumData::IS_SUM_TYPE)) {
      u32 enumIndex = 0xFFFFFFFF;
      for (u32 i = 0; i < symbols->enums.count; i++) {
        if (&symbols->enums[i] == resolvedEnum) {
          enumIndex = i;
          break;
        }
      }
      if (enumIndex != 0xFFFFFFFF) {
        u32 canonicalHash = resolvedEnum->name.nameHash;
        auto canonIt = structTypeMap.find(canonicalHash);
        if (canonIt != structTypeMap.end()) {
          if (typeNameHash != canonicalHash) {
            structTypeMap[typeNameHash] = canonIt->second;
          }
          return program.structTypes[canonIt->second].nameHash;
        }

        u32 result = RegisterEnumAsStructType(canonicalHash, enumIndex);
        if (result != 0 && typeNameHash != canonicalHash) {
          structTypeMap[typeNameHash] = structTypeMap[canonicalHash];
        }
        return result;
      }
    }

    // Try looking up via global custom type registry
    StructData *structData = g_customTypes.LookupType(typeNameHash);
    if (structData) {
      return RegisterStructTypeFromData(typeNameHash, structData);
    }
    if (const StdLib::BuiltinStructType *builtin =
            StdLib::FindBuiltinStructType(typeNameHash)) {
      return RegisterBuiltinStructType(*builtin);
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
    program.structFieldBounds[fieldOffset + i] = field.bounds;

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
        // The struct is padded to its alignment, so the next member can't
        // start inside it. Arrays keep their element stride below.
        if (field.arraySize == 0) fieldSize = (fieldSize + 15) & ~15u;
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
    program.structFieldBounds[fieldOffset + i] = field.bounds;

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
        // The struct is padded to its alignment, so the next member can't
        // start inside it. Arrays keep their element stride below.
        if (field.arraySize == 0) fieldSize = (fieldSize + 15) & ~15u;
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

inline u32
IRLowering::RegisterBuiltinStructType(const StdLib::BuiltinStructType &builtin) {
  if (program.structTypeCount >= program.structTypeCapacity) {
    return 0;
  }

  const u32 fieldCount = builtin.fieldCount;
  u32 typeNameHash = builtin.nameHash;
  auto existing = structTypeMap.find(typeNameHash);
  if (existing != structTypeMap.end()) {
    return typeNameHash;
  }

  u32 structIdx = program.structTypeCount++;
  structTypeMap[typeNameHash] = structIdx;
  ReverseLookup::Register(typeNameHash, builtin.name);

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
    const CoreType fieldType = builtin.fields[i].type;
    u32 fieldNameHash = Utils::HashStr(builtin.fields[i].name);
    ReverseLookup::Register(fieldNameHash, builtin.fields[i].name);

    program.structFieldTypes[fieldOffset + i] =
        static_cast<u16>(fieldType);
    program.structFieldNameHashes[fieldOffset + i] = fieldNameHash;
    program.structFieldTypeHashes[fieldOffset + i] = 0;
    program.structFieldArraySizes[fieldOffset + i] = 0;

    u32 alignment = GetTypeAlignment(fieldType);
    currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
    program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
    currentOffset += GetTypeSize(fieldType);
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

inline bool IRLowering::FindStructField(u32 structTypeHash, u32 fieldNameHash,
                                        u32 *outFieldIndex,
                                        CoreType *outFieldType,
                                        u32 *outFieldTypeHash) {
  if (outFieldIndex) *outFieldIndex = 0xFFFFFFFF;
  if (outFieldType) *outFieldType = CoreType::INVALID;
  if (outFieldTypeHash) *outFieldTypeHash = 0;
  if (structTypeHash == 0) return false;

  u32 canonicalHash = LookupOrRegisterStructType(structTypeHash);
  u32 lookupHash = canonicalHash != 0 ? canonicalHash : structTypeHash;
  auto structIt = structTypeMap.find(lookupHash);
  if (structIt == structTypeMap.end()) {
    structIt = structTypeMap.find(structTypeHash);
  }
  if (structIt == structTypeMap.end()) return false;

  const IRProgram::StructTypeInfo &info = program.structTypes[structIt->second];
  for (u32 i = 0; i < info.fieldCount; i++) {
    u32 offset = info.fieldOffset + i;
    if (program.structFieldNameHashes[offset] != fieldNameHash) continue;
    if (outFieldIndex) *outFieldIndex = i;
    if (outFieldType) {
      *outFieldType = static_cast<CoreType>(program.structFieldTypes[offset]);
    }
    if (outFieldTypeHash && program.structFieldTypeHashes) {
      *outFieldTypeHash = program.structFieldTypeHashes[offset];
    }
    return true;
  }

  return false;
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
  // A matrix takes one float slot per element, column by column.
  if (mask(type) & TypeMasks::MATRIX_TYPES) {
    u32 columns = type == CoreType::MAT4 ? 4 : type == CoreType::MAT3 ? 3 : 2;
    scalarType = CoreType::FLOAT;
    componentCount = columns * columns;
  }
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

inline CoreType IRLowering::EnumPayloadSlotType(u32 enumStructHash, u32 slot) {
  auto it = structTypeMap.find(enumStructHash);
  if (it == structTypeMap.end()) return CoreType::INVALID;
  const IRProgram::StructTypeInfo &info = program.structTypes[it->second];
  if (1 + slot >= info.fieldCount) return CoreType::INVALID;
  return static_cast<CoreType>(program.structFieldTypes[info.fieldOffset + 1 + slot]);
}

inline u16 IRLowering::EnumPayloadToSlots(NodeRef node, u16 reg, CoreType type,
                                          u32 enumStructHash, u32 slot) {
  CoreType component = GetScalarComponentType(type);
  if (type == CoreType::CUSTOM || type == CoreType::ENUM ||
      component == CoreType::FLOAT ||
      EnumPayloadSlotType(enumStructHash, slot) != CoreType::FLOAT) {
    return reg;
  }
  u16 bits = AllocateRegister();
  if (component == CoreType::BOOL) {
    if (type != CoreType::BOOL) {
      ReportErrorAt(node, "Error: a bool vector payload can't share its slots "
                          "with another variant's payload of another type; "
                          "reorder the payloads or use a uint vector\n");
      return 0xFFFF;
    }
    SetRegisterType(bits, CoreType::FLOAT);
    builder.EmitInstruction(OP_SELECT, bits, builder.EmitConstant(0.0f),
                            builder.EmitConstant(1.0f), reg);
    return bits;
  }
  SetRegisterType(bits, GetVectorType(CoreType::FLOAT, GetVectorDimension(type)));
  builder.EmitInstruction(OP_BITCAST, bits, reg);
  return bits;
}

inline u16 IRLowering::ReadEnumPayloadSlot(u16 receiver, u32 slot, CoreType type) {
  CoreType storage = EnumPayloadSlotType(program.registerStructTypes[receiver], slot);
  bool bits = storage == CoreType::FLOAT && type != CoreType::FLOAT &&
              (type == CoreType::INT || type == CoreType::UINT || type == CoreType::BOOL);
  u16 field = AllocateRegister();
  SetRegisterType(field, bits ? CoreType::FLOAT : type);
  builder.EmitInstruction(OP_ENUM_FIELD, field, receiver, EmitConstantInt(slot));
  if (!bits) return field;
  u16 value = AllocateRegister();
  SetRegisterType(value, type);
  if (type == CoreType::BOOL) {
    builder.EmitInstruction(OP_FNE, value, field, builder.EmitConstant(0.0f));
  } else {
    builder.EmitInstruction(OP_BITCAST, value, field);
  }
  return value;
}

inline bool IRLowering::FlattenEnumPayload(
    NodeRef node, u16 value, CoreType type, u32 typeHash,
    std::vector<std::pair<u16, CoreType>> &scalars) {
  if (type == CoreType::CUSTOM && typeHash != 0 && !IsSumEnumTypeHash(typeHash)) {
    u32 structHash = LookupOrRegisterStructType(typeHash);
    auto it = structTypeMap.find(structHash);
    if (it == structTypeMap.end()) return true;
    const IRProgram::StructTypeInfo info = program.structTypes[it->second];
    for (u32 f = 0; f < info.fieldCount; f++) {
      u32 slot = info.fieldOffset + f;
      if (program.structFieldArraySizes[slot] != 0) {
        ReportErrorAt(node, "Error: an enum payload struct can't have array "
                            "fields; wrap the array in a separate variant\n");
        return false;
      }
      CoreType fieldType = static_cast<CoreType>(program.structFieldTypes[slot]);
      u32 fieldHash = program.structFieldTypeHashes[slot];
      u16 field = AllocateRegister();
      SetRegisterType(field, fieldType);
      if (fieldType == CoreType::CUSTOM) {
        u32 fieldStruct = LookupOrRegisterStructType(fieldHash);
        program.registerStructTypes[field] = fieldStruct != 0 ? fieldStruct : fieldHash;
      }
      builder.EmitInstruction(OP_STRUCT_EXTRACT, field, value, static_cast<u16>(f));
      program.metadata[builder.currentInstruction - 1] = structHash;
      if (!FlattenEnumPayload(node, field, fieldType, fieldHash, scalars)) return false;
    }
    return true;
  }
  if (mask(type) & TypeMasks::MATRIX_TYPES) {
    u32 columns = type == CoreType::MAT4 ? 4 : type == CoreType::MAT3 ? 3 : 2;
    CoreType columnType = GetVectorType(CoreType::FLOAT, columns);
    for (u32 c = 0; c < columns; c++) {
      u16 column = AllocateRegister();
      SetRegisterType(column, columnType);
      builder.EmitInstruction(OP_ARRAY_LOAD, column, value, EmitConstantInt(c));
      FlattenEnumPayload(node, column, columnType, 0, scalars);
    }
    return true;
  }
  u32 width = GetVectorDimension(type);
  if (width > 1 && type != CoreType::CUSTOM && type != CoreType::ENUM) {
    CoreType component = GetScalarComponentType(type);
    for (u32 c = 0; c < width; c++) {
      u16 scalar = AllocateRegister();
      SetRegisterType(scalar, component);
      builder.EmitInstruction(OP_VEC_EXTRACT, scalar, value, static_cast<u16>(c));
      scalars.push_back({scalar, component});
    }
    return true;
  }
  scalars.push_back({value, type});
  return true;
}

inline u16 IRLowering::ReadEnumPayloadValue(u16 receiver, u32 *slot, CoreType type,
                                            u32 typeHash) {
  if (type == CoreType::CUSTOM && typeHash != 0 && !IsSumEnumTypeHash(typeHash)) {
    u32 structHash = LookupOrRegisterStructType(typeHash);
    auto it = structTypeMap.find(structHash);
    u16 current = AllocateRegister();
    SetRegisterType(current, CoreType::CUSTOM);
    program.registerStructTypes[current] = structHash;
    AddUndefRegister(current, CoreType::CUSTOM);
    if (it == structTypeMap.end()) return current;
    const IRProgram::StructTypeInfo info = program.structTypes[it->second];
    for (u32 f = 0; f < info.fieldCount; f++) {
      u32 fieldSlot = info.fieldOffset + f;
      u16 field = ReadEnumPayloadValue(
          receiver, slot, static_cast<CoreType>(program.structFieldTypes[fieldSlot]),
          program.structFieldTypeHashes[fieldSlot]);
      u16 next = AllocateRegister();
      SetRegisterType(next, CoreType::CUSTOM);
      program.registerStructTypes[next] = structHash;
      builder.EmitInstruction(OP_STRUCT_INSERT, next, current, static_cast<u16>(f), field);
      program.metadata[builder.currentInstruction - 1] = structHash;
      current = next;
    }
    return current;
  }
  if (type == CoreType::CUSTOM || type == CoreType::ENUM) {
    // A nested sum enum is one aggregate slot.
    u16 value = AllocateRegister();
    SetRegisterType(value, type);
    u32 structHash = LookupOrRegisterStructType(typeHash);
    program.registerStructTypes[value] = structHash != 0 ? structHash : typeHash;
    builder.EmitInstruction(OP_ENUM_FIELD, value, receiver, EmitConstantInt((*slot)++));
    return value;
  }
  if (mask(type) & TypeMasks::MATRIX_TYPES) {
    u32 columns = type == CoreType::MAT4 ? 4 : type == CoreType::MAT3 ? 3 : 2;
    u16 parts[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    for (u32 c = 0; c < columns; c++)
      parts[c] = ReadEnumPayloadValue(receiver, slot, GetVectorType(CoreType::FLOAT, columns), 0);
    u16 value = AllocateRegister();
    SetRegisterType(value, type);
    builder.EmitInstruction(OP_MAT_CONSTRUCT, value, parts[0], parts[1], parts[2], parts[3]);
    program.metadata[builder.currentInstruction - 1] = columns;
    return value;
  }
  u32 width = GetVectorDimension(type);
  if (width < 2) return ReadEnumPayloadSlot(receiver, (*slot)++, type);
  u16 parts[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
  for (u32 c = 0; c < width; c++)
    parts[c] = ReadEnumPayloadSlot(receiver, (*slot)++, GetScalarComponentType(type));
  u16 value = AllocateRegister();
  SetRegisterType(value, type);
  builder.EmitInstruction(OP_VEC_CONSTRUCT, value, parts[0], parts[1], parts[2], parts[3]);
  program.metadata[builder.currentInstruction - 1] = width;
  return value;
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

  // A non-float payload field that shares a slot with another type is stored
  // entirely as float bits, so it is written and read back with one bitcast
  // (see EnumPayloadToSlots). Widening one field can widen a slot another
  // field shares, so repeat until nothing changes.
  for (bool changed = true; changed;) {
    changed = false;
    for (u32 v = 0; v < enumData.variants.count; v++) {
      const EnumData::Variant &variant = enumData.variants[v];
      u32 slot = 0;
      for (u32 t = 0; t < variant.associatedTypes.count; t++) {
        CoreType type = variant.associatedTypes[t];
        u32 hash = t < variant.associatedTypeHashes.count
                       ? variant.associatedTypeHashes[t] : 0;
        u32 count = GetEnumPayloadFieldCount(type, hash);
        bool shared = false;
        if (type != CoreType::CUSTOM && type != CoreType::ENUM &&
            GetScalarComponentType(type) != CoreType::FLOAT) {
          for (u32 i = slot; i < slot + count && i < MAX_ENUM_PAYLOAD_FIELDS; i++)
            shared |= payloadFieldTypes[i] == CoreType::FLOAT;
        }
        for (u32 i = slot; shared && i < slot + count && i < MAX_ENUM_PAYLOAD_FIELDS; i++) {
          changed |= payloadFieldTypes[i] != CoreType::FLOAT;
          payloadFieldTypes[i] = CoreType::FLOAT;
          payloadFieldTypeHashes[i] = 0;
        }
        slot += count;
      }
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
        fieldSize = (program.structTypes[nestedIt->second].totalSize + 15) & ~15u;
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
    // Do not enter the tagged-constant range or return the invalid marker.
    if (builder.nextRegister >= MAX_REGISTERS) {
        NodeRef node;
        node.packed = builder.currentSourceNode;
        if (!hadError) ReportErrorAt(node, "Shader register limit exceeded (32764 registers). Split or simplify the shader.\n");
        return 0;
    }
    u16 reg = builder.nextRegister++;
    program.registerTypes[reg] = static_cast<u16>(CoreType::FLOAT);
    program.registerStructTypes[reg] = 0;
    program.registerStorageInfo[reg] = 0;
    if (reg >= program.registerCount) {
        program.registerCount = reg + 1;
    }
    return reg;
}

inline u16 IRLowering::EmitConstantInt(u32 value) {
    u16 constant = builder.EmitConstantInteger(value, false);
    if (constant != 0xFFFF) return constant;
    NodeRef node;
    node.packed = builder.currentSourceNode;
    if (!hadError) ReportErrorAt(node, "Signed integer constant limit exceeded (8192 values). Split or simplify the shader.\n");
    return IR::MakeIntConstant(0);
}

// Fold only pure integer ASTs. In particular, do not infer constants from a
// mutable local's current register: a later loop iteration may change it.
inline bool TryFoldArrayIndex(AST* ast, NodeRef ref, s64* value, bool* isUnsigned,
                              u32 depth = 0) {
  if (!ref.IsValid() || depth > 128) return false;
  if (ref.Type() == ASTNodeType::LITERAL) {
    const LiteralValue& literal = ast->GetLiteral(ref).value;
    *isUnsigned = literal.type == LiteralValue::UINT;
    if (literal.type == LiteralValue::INT) *value = literal.intValue;
    else if (literal.type == LiteralValue::UINT) *value = literal.uintValue;
    else if (literal.type == LiteralValue::BOOL) *value = literal.boolValue;
    else return false;
    return true;
  }
  if (ref.Type() == ASTNodeType::UNARY_OP) {
    const UnaryOpData& op = ast->GetUnaryOp(ref);
    if (op.op != UnaryOpType::NEGATE && op.op != UnaryOpType::NOT &&
        op.op != UnaryOpType::BITWISE_NOT) return false;
    if (!TryFoldArrayIndex(ast, op.operand, value, isUnsigned, depth + 1)) return false;
    if (op.op == UnaryOpType::NOT) { *value = !*value; *isUnsigned = false; }
    else if (op.op == UnaryOpType::BITWISE_NOT)
      *value = *isUnsigned ? static_cast<s64>(~static_cast<u32>(*value))
                           : static_cast<s64>(~static_cast<s32>(*value));
    else if (*isUnsigned) *value = 0u - static_cast<u32>(*value);
    else { if (*value == INT32_MIN) return false; *value = -*value; }
    return true;
  }
  if (ref.Type() == ASTNodeType::FUNCTION_CALL) {
    const FunctionCallData& call = ast->GetFunctionCall(ref);
    bool toUint = call.name.nameHash == Utils::HashStr("uint");
    if ((!toUint && call.name.nameHash != Utils::HashStr("int")) ||
        call.arguments.count != 1 ||
        !TryFoldArrayIndex(ast, call.arguments[0], value, isUnsigned, depth + 1)) return false;
    *isUnsigned = toUint;
    *value = toUint ? static_cast<s64>(static_cast<u32>(*value))
                    : static_cast<s64>(static_cast<s32>(*value));
    return true;
  }
  if (ref.Type() == ASTNodeType::TERNARY_EXPRESSION) {
    const TernaryExprData& ternary = ast->GetTernaryExpression(ref);
    s64 condition; bool conditionUnsigned;
    if (!TryFoldArrayIndex(ast, ternary.condition, &condition, &conditionUnsigned, depth + 1)) return false;
    return TryFoldArrayIndex(ast, condition ? ternary.trueExpr : ternary.falseExpr,
                             value, isUnsigned, depth + 1);
  }
  if (ref.Type() != ASTNodeType::BINARY_OP) return false;
  const BinaryOpData& op = ast->GetBinaryOp(ref);
  s64 left, right; bool leftUnsigned, rightUnsigned;
  if (!TryFoldArrayIndex(ast, op.left, &left, &leftUnsigned, depth + 1)) return false;
  if ((op.op == BinaryOpType::AND && !left) || (op.op == BinaryOpType::OR && left)) {
    *value = left != 0; *isUnsigned = false; return true;
  }
  if (!TryFoldArrayIndex(ast, op.right, &right, &rightUnsigned, depth + 1)) return false;
  // Mixed signedness follows normal lowering, which can change with overloads;
  // leave those cases to runtime rather than guessing a conversion here.
  if (leftUnsigned != rightUnsigned) return false;
  *isUnsigned = leftUnsigned;
  switch (op.op) {
  case BinaryOpType::ADD: *value = left + right; break;
  case BinaryOpType::SUBTRACT: *value = left - right; break;
  case BinaryOpType::MULTIPLY:
    if (leftUnsigned) *value = static_cast<u32>(static_cast<u64>(left) * static_cast<u64>(right));
    else *value = left * right;
    break;
  case BinaryOpType::DIVIDE:
    if (!right) return false;
    *value = left / right; break;
  case BinaryOpType::MODULO:
    if (!right) return false;
    *value = left % right;
    break;
  case BinaryOpType::BITWISE_AND: *value = left & right; break;
  case BinaryOpType::BITWISE_OR: *value = left | right; break;
  case BinaryOpType::BITWISE_XOR: *value = left ^ right; break;
  case BinaryOpType::LEFT_SHIFT:
    if (right < 0 || right >= 32) return false;
    *value = static_cast<u32>(left) << right;
    if (!leftUnsigned) *value = static_cast<s32>(*value);
    break;
  case BinaryOpType::RIGHT_SHIFT:
    if (right < 0 || right >= 32) return false;
    *value = left >> right; break;
  case BinaryOpType::EQUALS: *value = left == right; *isUnsigned = false; break;
  case BinaryOpType::NOT_EQUALS: *value = left != right; *isUnsigned = false; break;
  case BinaryOpType::LESS: *value = left < right; *isUnsigned = false; break;
  case BinaryOpType::LESS_EQUAL: *value = left <= right; *isUnsigned = false; break;
  case BinaryOpType::GREATER: *value = left > right; *isUnsigned = false; break;
  case BinaryOpType::GREATER_EQUAL: *value = left >= right; *isUnsigned = false; break;
  case BinaryOpType::AND: *value = left && right; *isUnsigned = false; break;
  case BinaryOpType::OR: *value = left || right; *isUnsigned = false; break;
  default: return false;
  }
  if (*isUnsigned) *value = static_cast<u32>(*value);
  else if (*value < INT32_MIN || *value > INT32_MAX) return false;
  return true;
}

inline bool IRLowering::CheckConstComponentIndex(u16 indexReg, NodeRef indexExpr,
                                                 CoreType containerType) {
  bool matrix = (mask(containerType) & TypeMasks::MATRIX_TYPES) != 0;
  u32 length = containerType == CoreType::MAT4 ? 4 :
               containerType == CoreType::MAT3 ? 3 :
               containerType == CoreType::MAT2 ? 2 : GetVectorDimension(containerType);
  if (length < 2) return true;
  s64 index;
  if (!TryGetConstIndex(indexReg, indexExpr, &index)) return true;
  if (index >= 0 && index < static_cast<s64>(length)) return true;
  char msg[192];
  snprintf(msg, sizeof(msg),
           "Error: index %lld is out of bounds for %s, which has %u %s\n",
           static_cast<long long>(index), SourceTypeName(containerType).c_str(),
           length, matrix ? "columns" : "components");
  ReportErrorAt(indexExpr, msg);
  return false;
}

// Reports an error when a constant index into a local array of known length
// is statically out of bounds (negative or >= length). Returns true when the
// access is fine or cannot be checked statically.
inline bool IRLowering::CheckConstArrayIndexBounds(u16 baseReg, u16 indexReg,
                                                  NodeRef indexExpr) {
  if (baseReg >= MAX_REGISTERS) return true;
  u32 info = program.registerStorageInfo[baseReg];
  if (!(info & IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) return true;
  u32 arrayIdx = info >> IR::IRProgram::STORAGE_BINDING_SHIFT;
  if (arrayIdx >= program.localArrayCount) return true;
  return CheckConstIndexInBounds(indexReg, indexExpr,
                                 program.localArraySizes[arrayIdx]);
}

// Reads the value of an index that is a constant register or folds to one.
inline bool IRLowering::TryGetConstIndex(u16 indexReg, NodeRef indexExpr,
                                         s64* value) {
    if (IR::IsIntConstant(indexReg)) {
        u32 slot = IR::IntConstantIndex(indexReg);
        if (slot >= program.intCount) return false;
        *value = static_cast<s32>(program.intConstants[slot]);
        return true;
    }
    if (IR::IsUintConstant(indexReg)) {
        u32 slot = IR::UintConstantIndex(indexReg);
        if (slot >= program.uintCount) return false;
        *value = program.uintConstants[slot];
        return true;
    }
    bool isUnsigned;
    return !indexExpr.IsNull() && TryFoldArrayIndex(ast, indexExpr, value, &isUnsigned);
}

// Reports an error when a constant index is negative or >= length. Returns
// true when the index is fine or not a constant.
inline bool IRLowering::CheckConstIndexInBounds(u16 indexReg, NodeRef indexExpr,
                                                u32 length) {
  if (length == 0) return true;

  s64 indexValue;
  if (!TryGetConstIndex(indexReg, indexExpr, &indexValue)) return true;

  if (indexValue < 0 || indexValue >= static_cast<s64>(length)) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Error: array index %lld is out of bounds for array of length "
             "%u\n",
             static_cast<long long>(indexValue), length);
    ReportError(msg);
    return false;
  }
  return true;
}

inline u16 IRLowering::EmitConstantUint(u32 value) {
    u16 constant = builder.EmitConstantInteger(value, true);
    if (constant != 0xFFFF) return constant;
    NodeRef node;
    node.packed = builder.currentSourceNode;
    if (!hadError) ReportErrorAt(node, "Unsigned integer constant limit exceeded (8192 values). Split or simplify the shader.\n");
    return IR::MakeUintConstant(0);
}

inline u16 IRLowering::TryConvertScalarConstant(u16 reg, CoreType targetType) {
  if (reg == 0xFFFF || !IR::IsConstant(reg)) return 0xFFFF;
  CoreType sourceType = GetRegisterType(reg);
  if (sourceType == targetType) return reg;
  double value;
  if (sourceType == CoreType::FLOAT) value = program.floatConstants[IR::FloatConstantIndex(reg)];
  else if (sourceType == CoreType::INT) value = static_cast<s32>(program.intConstants[IR::IntConstantIndex(reg)]);
  else if (sourceType == CoreType::UINT) value = program.uintConstants[IR::UintConstantIndex(reg)];
  else if (sourceType == CoreType::BOOL) value = program.boolConstants[IR::BoolConstantIndex(reg)];
  else return 0xFFFF;
  if (targetType == CoreType::FLOAT) return builder.EmitConstant(static_cast<float>(value));
  if (targetType != CoreType::INT && targetType != CoreType::UINT) return 0xFFFF;
  if (sourceType == CoreType::FLOAT) {
    // Float-to-integer conversion is undefined outside the target's range.
    // Do not invoke an undefined host cast while compiling those expressions.
    value = std::trunc(value);
    if (!std::isfinite(value) ||
        (targetType == CoreType::INT && (value < INT32_MIN || value > INT32_MAX)) ||
        (targetType == CoreType::UINT && (value < 0 || value > UINT32_MAX))) return 0xFFFF;
  }
  u32 bits = sourceType == CoreType::INT ||
                     (sourceType == CoreType::FLOAT && targetType == CoreType::INT)
                 ? static_cast<u32>(static_cast<s32>(value))
                 : static_cast<u32>(value);
  return targetType == CoreType::INT ? EmitConstantInt(bits) : EmitConstantUint(bits);
}

inline u16 IRLowering::TryFoldScalarArithmetic(OpCode op, u16 left, u16 right) {
  if (left == 0xFFFF || right == 0xFFFF || !IR::IsConstant(left) ||
      !IR::IsConstant(right) || GetRegisterType(left) != GetRegisterType(right)) return 0xFFFF;
  CoreType type = GetRegisterType(left);
  if (type == CoreType::FLOAT) {
    float a = program.floatConstants[IR::FloatConstantIndex(left)];
    float b = program.floatConstants[IR::FloatConstantIndex(right)];
    float result;
    switch (op) {
    case OP_FADD: result = a + b; break;
    case OP_FSUB: result = a - b; break;
    case OP_FMUL: result = a * b; break;
    case OP_FDIV: if (b == 0) return 0xFFFF; result = a / b; break;
    default: return 0xFFFF;
    }
    if (!std::isfinite(result)) return 0xFFFF;
    return builder.EmitConstant(result);
  }
  if (type == CoreType::UINT) {
    u32 a = program.uintConstants[IR::UintConstantIndex(left)];
    u32 b = program.uintConstants[IR::UintConstantIndex(right)];
    u32 result;
    switch (op) {
    case OP_IADD: result = a + b; break;
    case OP_ISUB: result = a - b; break;
    case OP_IMUL: result = a * b; break;
    case OP_IDIV: if (!b) return 0xFFFF; result = a / b; break;
    case OP_IMOD: if (!b) return 0xFFFF; result = a % b; break;
    default: return 0xFFFF;
    }
    return EmitConstantUint(result);
  }
  if (type == CoreType::INT) {
    s64 a = static_cast<s32>(program.intConstants[IR::IntConstantIndex(left)]);
    s64 b = static_cast<s32>(program.intConstants[IR::IntConstantIndex(right)]);
    s64 result;
    switch (op) {
    case OP_IADD: result = a + b; break;
    case OP_ISUB: result = a - b; break;
    case OP_IMUL: result = a * b; break;
    case OP_IDIV:
    case OP_IMOD:
      if (!b || (a == INT32_MIN && b == -1)) return 0xFFFF;
      result = op == OP_IDIV ? a / b : a % b;
      break;
    default: return 0xFFFF;
    }
    if (result < INT32_MIN || result > INT32_MAX) return 0xFFFF;
    return EmitConstantInt(static_cast<u32>(result));
  }
  return 0xFFFF;
}

// Call right after emitting the OP_VEC_CONSTRUCT that writes `reg`, a fresh
// temporary. Records it when every component is a scalar constant.
inline void IRLowering::RecordConstantVector(u16 reg) {
  u32 inst = builder.currentInstruction - 1;
  CoreType type = GetRegisterType(reg);
  CoreType scalarType = GetScalarComponentType(type);
  u32 width = GetVectorDimension(type);
  u32 count = program.metadata[inst];
  if (width < 2 || (count != 1 && count != width)) return;
  for (u32 i = 0; i < count; i++) {
    u16 component = program.GetOperand(inst, i);
    if (component == 0xFFFF || !IR::IsConstant(component) ||
        GetRegisterType(component) != scalarType) return;
  }
  constantVectors[reg] = inst;
}

inline bool IRLowering::GetConstantVectorComponents(u16 reg, u16 components[4],
                                                    u32 *width) {
  auto it = constantVectors.find(reg);
  if (it == constantVectors.end()) return false;
  *width = GetVectorDimension(GetRegisterType(reg));
  bool splat = program.metadata[it->second] == 1;
  for (u32 i = 0; i < *width; i++) {
    components[i] = program.GetOperand(it->second, splat ? 0 : i);
  }
  return true;
}

// Folds componentwise arithmetic on two constant vectors (a scalar operand is
// already splatted by then) into a new constant vector.
inline u16 IRLowering::TryFoldVectorArithmetic(OpCode op, u16 left, u16 right) {
  u16 a[4], b[4], folded[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
  u32 width, rightWidth;
  if (!GetConstantVectorComponents(left, a, &width) ||
      !GetConstantVectorComponents(right, b, &rightWidth) ||
      width != rightWidth || GetRegisterType(left) != GetRegisterType(right)) {
    return 0xFFFF;
  }
  for (u32 i = 0; i < width; i++) {
    folded[i] = TryFoldScalarArithmetic(op, a[i], b[i]);
    if (folded[i] == 0xFFFF) return 0xFFFF;
  }
  u16 dest = AllocateRegister();
  builder.EmitInstruction(OP_VEC_CONSTRUCT, dest, folded[0], folded[1],
                          folded[2], folded[3]);
  program.metadata[builder.currentInstruction - 1] = width;
  SetRegisterType(dest, GetRegisterType(left));
  RecordConstantVector(dest);
  return dest;
}

inline u16 IRLowering::ConvertRegisterToType(u16 reg, CoreType targetType) {
  CoreType sourceType = GetRegisterType(reg);
  if (sourceType == targetType) {
    return reg;
  }

  OpCode convOp = OP_NOP;
  if (targetType == CoreType::FLOAT) {
    if (sourceType == CoreType::INT) {
      convOp = OP_I2F;
    } else if (sourceType == CoreType::UINT) {
      convOp = OP_U2F;
    }
  } else if (targetType == CoreType::INT) {
    if (sourceType == CoreType::UINT) convOp = OP_U2I;
    else if (sourceType == CoreType::FLOAT) convOp = OP_F2I;
  } else if (targetType == CoreType::UINT) {
    if (sourceType == CoreType::INT) convOp = OP_I2U;
    else if (sourceType == CoreType::FLOAT) convOp = OP_F2U;
  }

  if (convOp == OP_NOP) {
    return reg;
  }

  u16 constant = TryConvertScalarConstant(reg, targetType);
  if (constant != 0xFFFF) return constant;
  u16 converted = AllocateRegister();
  builder.EmitInstruction(convOp, converted, reg);
  SetRegisterType(converted, targetType);
  return converted;
}

// Type name as written in source, for diagnostics.
inline std::string IRLowering::SourceTypeName(CoreType type) {
  std::string name = CoreTypeToString(type);
  for (char &c : name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return name;
}

// A store converts between scalar kinds (ConvertRegisterToType) but never
// changes a vector's width or component type, splats a scalar or collapses a
// vector: each takes an explicit swizzle or constructor. Reports the mismatch
// at `node` and returns false; matrices and aggregates are checked elsewhere.
inline bool IRLowering::CheckStoreShape(NodeRef node, u16 valueReg,
                                        CoreType targetType,
                                        u32 targetStructHash) {
  CoreType sourceType = GetRegisterType(valueReg);
  if (sourceType == CoreType::CUSTOM && targetType == CoreType::CUSTOM &&
      targetStructHash != 0 && IR::IsRegister(valueReg) &&
      valueReg < program.registerCount) {
    u32 sourceStructHash = program.registerStructTypes[valueReg];
    if (sourceStructHash != 0 && sourceStructHash != targetStructHash) {
      u32 source = LookupOrRegisterStructType(sourceStructHash);
      u32 target = LookupOrRegisterStructType(targetStructHash);
      if (source != 0 && target != 0 && source != target) {
        std::string message = "Error: cannot assign a value of struct " +
                              ReverseLookup::GetString(source) + " to struct " +
                              ReverseLookup::GetString(target) + "\n";
        ReportErrorAt(node, message.c_str());
        return false;
      }
    }
  }
  if (sourceType == targetType) return true;
  if ((mask(sourceType) & TypeMasks::MATRIX_TYPES) &&
      (mask(targetType) & TypeMasks::MATRIX_TYPES)) {
    std::string message = "Error: cannot implicitly convert " +
                           SourceTypeName(sourceType) + " to " +
                           SourceTypeName(targetType) +
                           "; matrix sizes must match\n";
    ReportErrorAt(node, message.c_str());
    return false;
  }
  constexpr TypeMask vectors = TypeMasks::VECTOR_TYPES | TypeMasks::BOOL_VECTORS;
  constexpr TypeMask values = vectors | TypeMasks::SCALAR_TYPES;
  bool sourceVector = (mask(sourceType) & vectors) != 0;
  bool targetVector = (mask(targetType) & vectors) != 0;
  if (!(mask(sourceType) & values) || !(mask(targetType) & values) ||
      (!sourceVector && !targetVector)) {
    return true;
  }
  std::string source = SourceTypeName(sourceType);
  std::string target = SourceTypeName(targetType);
  u32 sourceWidth = GetVectorDimension(sourceType);
  u32 targetWidth = GetVectorDimension(targetType);
  char msg[256];
  if (!targetVector) {
    snprintf(msg, sizeof(msg),
             "Error: cannot implicitly convert %s to %s; select a component "
             "such as .x\n",
             source.c_str(), target.c_str());
  } else if (!sourceVector) {
    snprintf(msg, sizeof(msg),
             "Error: cannot implicitly convert %s to %s; use %s(value) to "
             "splat a scalar\n",
             source.c_str(), target.c_str(), target.c_str());
  } else if (sourceWidth > targetWidth) {
    snprintf(msg, sizeof(msg),
             "Error: cannot implicitly convert %s to %s; vector widths must "
             "match, use a swizzle such as .%.*s to narrow\n",
             source.c_str(), target.c_str(), static_cast<int>(targetWidth),
             "xyzw");
  } else if (sourceWidth < targetWidth) {
    snprintf(msg, sizeof(msg),
             "Error: cannot implicitly convert %s to %s; vector widths must "
             "match, use a constructor such as %s(value, ...) to widen\n",
             source.c_str(), target.c_str(), target.c_str());
  } else {
    snprintf(msg, sizeof(msg),
             "Error: cannot implicitly convert %s to %s; vector component "
             "types must match, convert with %s(...)\n",
             source.c_str(), target.c_str(), target.c_str());
  }
  ReportErrorAt(node, msg);
  return false;
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
    // A constant's operand code says its type. The unused marker 0xFFFF
    // reads as bool, as it always has.
    if (IR::IsBoolConstant(reg) || reg == IR::UnusedOperand)
        return CoreType::BOOL;
    if (IR::IsFloatConstant(reg))
        return CoreType::FLOAT;
    if (IR::IsIntConstant(reg))
        return CoreType::INT;
    if (IR::IsUintConstant(reg))
        return CoreType::UINT;

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

inline CoreType IRLowering::LookupCoreType(u32 typeHash) const {
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

  typeHash = SymbolTable::ResolveTypeAliasHashInScope(
      symbols, typeHash, AliasOwnerKind(), AliasOwnerModuleIndex());

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

  const EnumData *resolvedEnum =
      SymbolTable::ResolveEnumDataByHash(symbols, typeHash);
  if (resolvedEnum) {
    if (resolvedEnum->flags & EnumData::IS_SUM_TYPE) {
      if (outCustomHash) {
        *outCustomHash = resolvedEnum->name.nameHash;
      }
      return CoreType::CUSTOM;
    }

    CoreType baseType = resolvedEnum->underlyingType;
    return baseType == CoreType::INVALID ? CoreType::INT : baseType;
  }

  if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
    if (outCustomHash) {
      // Use the struct's unqualified name hash for consistency
      const StructData &structData = symbols->structs[sym->index];
      *outCustomHash = structData.name.nameHash;
    }
    return CoreType::CUSTOM;
  }

  std::string typeName = ReverseLookup::GetString(typeHash);
  size_t scopePos = typeName.find("::");
  if (scopePos != std::string::npos && typeName.rfind("<hash:", 0) != 0) {
    std::string moduleName = typeName.substr(0, scopePos);
    std::string localTypeName = typeName.substr(scopePos + 2);
    u32 moduleHash = SymbolTable::ResolveModuleNameHashInScope(
        const_cast<SymbolTableData *>(symbols),
        Utils::HashStr(moduleName.c_str()),
        AliasOwnerKind(), AliasOwnerModuleIndex());
    u32 localHash = Utils::HashStr(localTypeName.c_str());

    auto lookupModuleType = [&](const char *prefix) -> Symbol * {
      std::string internalName;
      internalName.reserve(2 + 10 + 10);
      internalName.append("m").append(std::to_string(moduleHash));
      internalName.append(prefix).append(std::to_string(localHash));
      return SymbolTable::LookupByHash(
          const_cast<SymbolTableData *>(symbols),
          Utils::HashStr(internalName.c_str()));
    };

    sym = lookupModuleType("::s");
    if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
      if (outCustomHash) {
        *outCustomHash = localHash;
      }
      return CoreType::CUSTOM;
    }

    sym = lookupModuleType("::e");
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
      return baseType == CoreType::INVALID ? CoreType::INT : baseType;
    }
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


} // namespace BWSL::IR
