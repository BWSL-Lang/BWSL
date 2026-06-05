// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Type IDs, pointer/function/struct/image types, and sampled texture loading.


#ifdef BWSL_CLANGD
namespace BWSL {
#endif

u32 SPIRVBuilder::GetVectorTypeId(CoreType base, u32 components) {
  // Check if we can use predefined types
  if (base == CoreType::FLOAT) {
    switch (components) {
    case 2:
      return GetTypeId(CoreType::FLOAT2);
    case 3:
      return GetTypeId(CoreType::FLOAT3);
    case 4:
      return GetTypeId(CoreType::FLOAT4);
    }
  }
  // TODO: Handle other base types and dynamic vector creation
  return 0;
}

u32 SPIRVBuilder::GetPointerTypeId(u32 type_id, spv::StorageClass storage) {
  // Hash the pointer type for deduplication
  u32 hash = type_id ^ (static_cast<u32>(storage) << 16);

  // Check composite types for existing pointer
  for (u32 i = 0; i < compositeTypeCount; i++) {
    if (compositeTypeHashes[i] == hash) {
      return compositeTypeIds[i];
    }
  }

  // Create new pointer type
  u32 ptr_id = AllocateId();
  u32 ops[] = {ptr_id, storage, type_id};
  EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);

  // Cache it
  compositeTypeIds[compositeTypeCount] = ptr_id;
  compositeTypeHashes[compositeTypeCount] = hash;
  compositeTypeCount++;

  return ptr_id;
}

u32 SPIRVBuilder::GetFunctionTypeId(u32 return_type, u32 *param_types,
                                    u32 param_count) {
  // TODO: Implement function type creation and deduplication
  u32 func_type_id = AllocateId();

  // Build operands: type_id, return_type, param0, param1, ...
  u32 *ops = (u32 *)arena->Allocate((2 + param_count) * sizeof(u32));
  ops[0] = func_type_id;
  ops[1] = return_type;
  memcpy(&ops[2], param_types, param_count * sizeof(u32));

  EmitToSection(&typesConstants, spv::OpTypeFunction, ops, 2 + param_count);
  return func_type_id;
}

u32 SPIRVBuilder::GetStructTypeId(u32 structTypeHash) {
  // Check if we already have this struct type
  for (u32 i = 0; i < structTypeCount; i++) {
    if (structTypeHashes[i] == structTypeHash && structTypeIds[i] != 0) {
      return structTypeIds[i];
    }
  }

  // Find the struct type info in IR
  if (!ir || !ir->structTypes) {
    return 0;
  }

  const IR::IRProgram::StructTypeInfo *structInfo = nullptr;
  u32 structIdx = 0;
  for (u32 i = 0; i < ir->structTypeCount; i++) {
    if (ir->structTypes[i].nameHash == structTypeHash) {
      structInfo = &ir->structTypes[i];
      structIdx = i;
      break;
    }
  }

  if (!structInfo || structInfo->fieldCount == 0) {
    return 0;
  }

  // Allocate the struct type ID
  u32 struct_type_id = AllocateId();

  // Build OpTypeStruct: result_id, member_type_0, member_type_1, ...
  u32 fieldCount = structInfo->fieldCount;
  u32 *ops = (u32 *)arena->Allocate((1 + fieldCount) * sizeof(u32));
  ops[0] = struct_type_id;

  // Get SPIR-V type IDs for each field, handling arrays
  for (u32 i = 0; i < fieldCount; i++) {
    CoreType fieldType = static_cast<CoreType>(
        ir->structFieldTypes[structInfo->fieldOffset + i]);
    u32 fieldTypeId = 0;
    if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
        ir->structFieldTypeHashes) {
      u32 fieldTypeHash =
          ir->structFieldTypeHashes[structInfo->fieldOffset + i];
      if (fieldTypeHash != 0) {
        fieldTypeId = GetStructTypeId(fieldTypeHash);
      }
    } else {
      fieldTypeId = GetTypeId(fieldType);
    }
    if (fieldTypeId == 0) {
      // Fallback to float to keep the type graph valid
      fieldTypeId = GetTypeId(CoreType::FLOAT);
    }

    // Check if this field is an array
    u32 arraySize = 0;
    if (ir->structFieldArraySizes) {
      arraySize = ir->structFieldArraySizes[structInfo->fieldOffset + i];
    }

    if (arraySize > 0) {
      u32 baseTypeId = fieldTypeId;

      // Create OpTypeArray wrapping the base type
      u32 arrayTypeId = AllocateId();
      u32 lengthConstId = GetIntConstantId(arraySize, true);
      u32 arrayOps[] = {arrayTypeId, baseTypeId, lengthConstId};
      EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);

      // Calculate element stride for std140 layout (aligned to 16 bytes)
      u32 fieldSize = 0;
      if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
          ir->structFieldTypeHashes) {
        // For custom structs, use totalSize from IR
        u32 fieldTypeHash =
            ir->structFieldTypeHashes[structInfo->fieldOffset + i];
        if (fieldTypeHash != 0) {
          for (u32 j = 0; j < ir->structTypeCount; j++) {
            if (ir->structTypes[j].nameHash == fieldTypeHash) {
              fieldSize = ir->structTypes[j].totalSize;
              break;
            }
          }
        }
      }
      if (fieldSize == 0) {
        // Fall back to core type size
        switch (fieldType) {
        case CoreType::FLOAT:
        case CoreType::INT:
        case CoreType::UINT:
        case CoreType::BOOL:
          fieldSize = 4;
          break;
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
          fieldSize = 8;
          break;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
          fieldSize = 12;
          break;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
          fieldSize = 16;
          break;
        case CoreType::MAT2:
          fieldSize = 32;
          break;
        case CoreType::MAT3:
          fieldSize = 48;
          break;
        case CoreType::MAT4:
          fieldSize = 64;
          break;
        default:
          fieldSize = 4;
          break;
        }
      }
      // std430 layout: scalars and vec2 use natural stride, vec3+ rounds to 16
      u32 stride;
      if (fieldSize <= 8) {
        stride = fieldSize; // Natural size for scalars (4) and vec2 (8)
      } else {
        stride = (fieldSize + 15) & ~15; // Round to 16 for vec3, vec4, mat
      }
      u32 stride_val[] = {stride};
      EmitDecoration(arrayTypeId, spv::DecorationArrayStride, stride_val, 1);

      fieldTypeId = arrayTypeId; // Use array type instead of base type
    }

    ops[1 + i] = fieldTypeId;
  }

  EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 1 + fieldCount);

  // Emit struct name for debugging (use ReverseLookup to convert hash back to
  // string)
  if (emitDebugNames) {
    std::string structName = ReverseLookup::GetString(structTypeHash);
    if (!structName.empty()) {
      EmitName(struct_type_id, structName.c_str());
    }
  }

  // Emit member offset decorations and member names for std140 layout
  for (u32 i = 0; i < fieldCount; i++) {
    u32 byteOffset = ir->structFieldByteOffsets[structInfo->fieldOffset + i];
    EmitMemberDecoration(struct_type_id, i, spv::DecorationOffset, byteOffset);

    // Add ColMajor and MatrixStride decorations for matrix fields
    CoreType fieldType = static_cast<CoreType>(
        ir->structFieldTypes[structInfo->fieldOffset + i]);
    if (fieldType == CoreType::MAT2 || fieldType == CoreType::MAT3 ||
        fieldType == CoreType::MAT4) {
      // Emit ColMajor decoration (no value)
      u32 col_major_ops[] = {struct_type_id, i, spv::DecorationColMajor};
      EmitToSection(&decorations, spv::OpMemberDecorate, col_major_ops, 3);
      // Emit MatrixStride decoration (16 bytes per column for std140)
      u32 matrix_stride_ops[] = {struct_type_id, i, spv::DecorationMatrixStride,
                                 16};
      EmitToSection(&decorations, spv::OpMemberDecorate, matrix_stride_ops, 4);
    }

    // Emit member name for debugging
    if (emitDebugNames && ir->structFieldNameHashes) {
      u32 fieldNameHash =
          ir->structFieldNameHashes[structInfo->fieldOffset + i];
      std::string fieldName = ReverseLookup::GetString(fieldNameHash);
      if (!fieldName.empty()) {
        EmitMemberName(struct_type_id, i, fieldName.c_str());
      }
    }
  }

  // Cache the struct type and its field type IDs
  if (structTypeCount < 64) {
    u32 structIdx = structTypeCount;
    structTypeHashes[structIdx] = structTypeHash;
    structTypeIds[structIdx] = struct_type_id;

    // Store field type IDs for later lookup
    for (u32 i = 0; i < fieldCount && i < MAX_FIELDS_PER_STRUCT; i++) {
      structFieldTypeIds[structIdx * MAX_FIELDS_PER_STRUCT + i] = ops[1 + i];
    }

    structTypeCount++;
  }

  return struct_type_id;
}

// ============= Texture Type Management =============
u32 SPIRVBuilder::GetImageTypeId() {
  if (imageTypeId != 0)
    return imageTypeId;

  // OpTypeImage: sampled_type, dim, depth, arrayed, ms, sampled, format
  // For a typical 2D sampled texture: float, Dim2D, 0, 0, 0, 1, Unknown
  imageTypeId = AllocateId();
  u32 float_type = GetTypeId(CoreType::FLOAT);
  u32 ops[] = {imageTypeId,
               float_type,
               static_cast<u32>(spv::Dim2D),
               0, // depth (not depth texture)
               0, // arrayed (not array)
               0, // ms (not multisampled)
               1, // sampled (used with sampler)
               static_cast<u32>(spv::ImageFormatUnknown)};
  EmitToSection(&typesConstants, spv::OpTypeImage, ops, 8);

  return imageTypeId;
}

u32 SPIRVBuilder::GetSamplerTypeId() {
  if (samplerTypeId != 0)
    return samplerTypeId;

  samplerTypeId = AllocateId();
  u32 ops[] = {samplerTypeId};
  EmitToSection(&typesConstants, spv::OpTypeSampler, ops, 1);

  return samplerTypeId;
}

u32 SPIRVBuilder::GetSampledImageTypeId() {
  if (sampledImageTypeId != 0)
    return sampledImageTypeId;

  // OpTypeSampledImage requires the image type
  u32 img_type = GetImageTypeId();
  sampledImageTypeId = AllocateId();
  u32 ops[] = {sampledImageTypeId, img_type};
  EmitToSection(&typesConstants, spv::OpTypeSampledImage, ops, 2);

  return sampledImageTypeId;
}

u32 SPIRVBuilder::GetArrayImageTypeId() {
  if (arrayImageTypeId != 0)
    return arrayImageTypeId;

  // OpTypeImage for 2D array sampled texture: float, Dim2D, 0, 1 (arrayed), 0,
  // 1, Unknown
  arrayImageTypeId = AllocateId();
  u32 float_type = GetTypeId(CoreType::FLOAT);
  u32 ops[] = {arrayImageTypeId,
               float_type,
               static_cast<u32>(spv::Dim2D),
               0, // depth (not depth texture)
               1, // arrayed (IS array texture)
               0, // ms (not multisampled)
               1, // sampled (used with sampler)
               static_cast<u32>(spv::ImageFormatUnknown)};
  EmitToSection(&typesConstants, spv::OpTypeImage, ops, 8);

  return arrayImageTypeId;
}

u32 SPIRVBuilder::GetArraySampledImageTypeId() {
  if (arraySampledImageTypeId != 0)
    return arraySampledImageTypeId;

  // OpTypeSampledImage requires the array image type
  u32 img_type = GetArrayImageTypeId();
  arraySampledImageTypeId = AllocateId();
  u32 ops[] = {arraySampledImageTypeId, img_type};
  EmitToSection(&typesConstants, spv::OpTypeSampledImage, ops, 2);

  return arraySampledImageTypeId;
}

u32 SPIRVBuilder::GetCubeImageTypeId() {
  if (cubeImageTypeId != 0)
    return cubeImageTypeId;

  // OpTypeImage for cube sampled texture: float, DimCube, 0, 0, 0, 1, Unknown
  cubeImageTypeId = AllocateId();
  u32 float_type = GetTypeId(CoreType::FLOAT);
  u32 ops[] = {cubeImageTypeId,
               float_type,
               static_cast<u32>(spv::DimCube),
               0, // depth (not depth texture)
               0, // arrayed (not array)
               0, // ms (not multisampled)
               1, // sampled (used with sampler)
               static_cast<u32>(spv::ImageFormatUnknown)};
  EmitToSection(&typesConstants, spv::OpTypeImage, ops, 8);

  return cubeImageTypeId;
}

u32 SPIRVBuilder::GetCubeSampledImageTypeId() {
  if (cubeSampledImageTypeId != 0)
    return cubeSampledImageTypeId;

  // OpTypeSampledImage requires the cube image type
  u32 img_type = GetCubeImageTypeId();
  cubeSampledImageTypeId = AllocateId();
  u32 ops[] = {cubeSampledImageTypeId, img_type};
  EmitToSection(&typesConstants, spv::OpTypeSampledImage, ops, 2);

  return cubeSampledImageTypeId;
}

void SPIRVBuilder::GetSampledTextureTypeIds(u16 texSlot, u32* sampledImageType,
                                            u32* imageType) {
  if (textureIsArray[texSlot]) {
    if (sampledImageType) *sampledImageType = GetArraySampledImageTypeId();
    if (imageType) *imageType = GetArrayImageTypeId();
  } else if (textureIsCubemap[texSlot]) {
    if (sampledImageType) *sampledImageType = GetCubeSampledImageTypeId();
    if (imageType) *imageType = GetCubeImageTypeId();
  } else {
    if (sampledImageType) *sampledImageType = GetSampledImageTypeId();
    if (imageType) *imageType = GetImageTypeId();
  }
}

bool SPIRVBuilder::LoadSampledTexture(u16 texReg, CoreType missingResultType,
                                      u32 dest, bool needImage,
                                      SampledTextureLoad* outLoad) {
  SampledTextureLoad load{};
  load.slot = texReg & 0x0FFF;
  load.variableId = textureIds[load.slot];

  if (load.variableId == 0) {
    Emit(spv::OpUndef, GetTypeId(missingResultType), dest);
    if (outLoad) *outLoad = load;
    return false;
  }

  GetSampledTextureTypeIds(load.slot, &load.sampledImageTypeId,
                           needImage ? &load.imageTypeId : nullptr);

  load.sampledImageId = AllocateId();
  Emit(spv::OpLoad, load.sampledImageTypeId, load.sampledImageId,
       load.variableId);

  if (needImage) {
    load.imageId = AllocateId();
    Emit(spv::OpImage, load.imageTypeId, load.imageId, load.sampledImageId);
  }

  if (outLoad) *outLoad = load;
  return true;
}

u32 SPIRVBuilder::GetStorageImageTypeId() {
  if (storageImageTypeId != 0)
    return storageImageTypeId;

  // OpTypeImage for 2D storage image (read/write): float, Dim2D, 0, 0, 0, 2,
  // Rgba32f sampled = 2 means storage image (not used with sampler) Using
  // Rgba32f format for compatibility, but we have
  // StorageImageWriteWithoutFormat capability
  storageImageTypeId = AllocateId();
  u32 float_type = GetTypeId(CoreType::FLOAT);
  u32 ops[] = {storageImageTypeId,
               float_type,
               static_cast<u32>(spv::Dim2D),
               0, // depth (not depth texture)
               0, // arrayed (not array)
               0, // ms (not multisampled)
               2, // sampled = 2 (storage image, not used with sampler)
               static_cast<u32>(spv::ImageFormatRgba32f)};
  EmitToSection(&typesConstants, spv::OpTypeImage, ops, 8);

  return storageImageTypeId;
}


#ifdef BWSL_CLANGD
} // namespace BWSL
#endif
