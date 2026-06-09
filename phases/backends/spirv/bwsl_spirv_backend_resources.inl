// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Resource declarations, image/buffer setup, descriptor bindings, and storage buffers.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

static bool IsMatrixType(CoreType type) {
  return type == CoreType::MAT2 || type == CoreType::MAT3 ||
         type == CoreType::MAT4;
}

static u8 GetVertexPullingBindingCount(const SPIRVBuilder::VertexPullingConfig& config) {
  switch (config.mode) {
  case SPIRVBuilder::VertexInputMode::SeparateBuffers:
    return static_cast<u8>(std::popcount(config.attributeMask));
  case SPIRVBuilder::VertexInputMode::UnifiedWithOffsets:
    return 2;
  case SPIRVBuilder::VertexInputMode::Interleaved:
  default:
    return 0;
  }
}

static u8 ResolveVertexPullingCollisionBinding(
    const SPIRVBuilder::VertexPullingConfig& config, u32 resourceSet,
    u8 binding) {
  if (resourceSet != config.descriptorSet) {
    return binding;
  }

  const u8 occupiedCount = GetVertexPullingBindingCount(config);
  if (occupiedCount == 0) {
    return binding;
  }

  if (binding >= config.baseBufferBinding) {
    return binding + occupiedCount;
  }

  return binding;
}

void SPIRVBuilder::DeclareResources() {
  // Declare resources based on IR analysis results
  // Each uniform binding in BWSL is a single typed value
  // (resources.modelMatrix, etc.) We create a struct wrapper for each to
  // satisfy SPIR-V UBO requirements

  // ============= Uniform Buffers =============
  for (u8 binding = 0; binding < 32; binding++) {
    if (!(analysis.usedUniformMask & (1 << binding)))
      continue;

    // Skip bindings that are also used as storage buffers - those are handled
    // separately
    if (analysis.usedStorageBufferMask & (1 << binding))
      continue;

    // Get the actual type from analysis (derived from IR register types)
    CoreType uniformType =
        static_cast<CoreType>(analysis.uniformTypes[binding]);
    if (uniformType == CoreType::VOID || uniformType == CoreType::INVALID) {
      // Fallback to float4 if type unknown
      uniformType = CoreType::FLOAT4;
    }

    // Get type ID - use GetStructTypeId for CUSTOM/ENUM types
    u32 member_type_id = 0;
    if (uniformType == CoreType::CUSTOM || uniformType == CoreType::ENUM) {
      u32 structHash = analysis.uniformTypeHashes[binding];
      if (structHash != 0) {
        member_type_id = GetStructTypeId(structHash);
      }
    }
    if (member_type_id == 0) {
      member_type_id = GetTypeId(uniformType);
    }

    // Create struct type with single member (SPIR-V requires struct for UBOs)
    u32 struct_type_id = AllocateId();
    {
      u32 ops[] = {struct_type_id, member_type_id};
      EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
    }

    // Decorate struct as Block (required for uniform buffers)
    EmitDecoration(struct_type_id, spv::DecorationBlock, nullptr, 0);

    // Member offset decoration (member 0 at offset 0)
    u32 offset_ops[] = {struct_type_id, 0, spv::DecorationOffset, 0};
    EmitToSection(&decorations, spv::OpMemberDecorate, offset_ops, 4);

    // Matrix-specific decorations
    if (IsMatrixType(uniformType)) {
      u32 col_major[] = {struct_type_id, 0, spv::DecorationColMajor};
      EmitToSection(&decorations, spv::OpMemberDecorate, col_major, 3);

      // MatrixStride = 16 (vec4 per column)
      u32 stride[] = {struct_type_id, 0, spv::DecorationMatrixStride, 16};
      EmitToSection(&decorations, spv::OpMemberDecorate, stride, 4);
    }

    // Create pointer type (Uniform storage class)
    u32 ptr_type_id = AllocateId();
    {
      u32 ops[] = {ptr_type_id, spv::StorageClassUniform, struct_type_id};
      EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
    }

    // Create the variable
    u32 var_id = AllocateId();
    {
      u32 ops[] = {ptr_type_id, var_id, spv::StorageClassUniform};
      EmitToSection(&globals, spv::OpVariable, ops, 3);
    }

    u32 set_val[] = {0};
    u8 actualBinding =
        ResolveVertexPullingCollisionBinding(vertexPullingConfig, set_val[0],
                                            binding);
    u32 bind_val[] = {actualBinding};
    EmitDecoration(var_id, spv::DecorationDescriptorSet, set_val, 1);
    EmitDecoration(var_id, spv::DecorationBinding, bind_val, 1);

    uniformBufferIds[binding] = var_id;
    bindingSets[resourceCount] = 0;
    bindingIndices[resourceCount] = actualBinding;
    resourceCount++;
  }

  // ============= Textures (combined image samplers) =============
  // First create shared image and sampled image types if any textures used
  u32 sampled_image_type_id = 0;
  u32 ptr_sampled_image_type = 0;
  u32 array_sampled_image_type_id = 0;
  u32 ptr_array_sampled_image_type = 0;
  u32 cube_sampled_image_type_id = 0;
  u32 ptr_cube_sampled_image_type = 0;

  // Check if we need array/cube texture types by looking up textures in symbol
  // table
  bool needsArrayTexture = false;
  bool needsCubeTexture = false;
  bool needsRegularTexture = false;

  if (analysis.usedTextureMask != 0 && symbols) {
    for (u32 binding = 0; binding < 32; binding++) {
      if (!(analysis.usedTextureMask & (1 << binding)))
        continue;

      // Look up texture in symbol table to check if it's an array or cubemap
      bool isArray = false;
      bool isCubemap = false;
      for (u32 r = 0; r < symbols->resources.count; r++) {
        const ResourceData &resData = symbols->resources[r];
        if (resData.bindingIndex == binding &&
            resData.type == ResourceBinding::Texture) {
          isArray = resData.isArrayTexture;
          isCubemap = resData.isCubemapTexture;
          break;
        }
      }
      textureIsArray[binding] = isArray;
      textureIsCubemap[binding] = isCubemap;
      if (isArray)
        needsArrayTexture = true;
      else if (isCubemap)
        needsCubeTexture = true;
      else
        needsRegularTexture = true;
    }
  }

  if (analysis.usedTextureMask != 0) {
    // Create regular 2D texture types if needed
    if (needsRegularTexture) {
      sampled_image_type_id = GetSampledImageTypeId();

      ptr_sampled_image_type = AllocateId();
      {
        u32 ops[] = {ptr_sampled_image_type, spv::StorageClassUniformConstant,
                     sampled_image_type_id};
        EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
      }
    }

    // Create 2D array texture types if needed
    if (needsArrayTexture) {
      array_sampled_image_type_id = GetArraySampledImageTypeId();

      ptr_array_sampled_image_type = AllocateId();
      {
        u32 ops[] = {ptr_array_sampled_image_type,
                     spv::StorageClassUniformConstant,
                     array_sampled_image_type_id};
        EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
      }
    }

    // Create cube texture types if needed
    if (needsCubeTexture) {
      cube_sampled_image_type_id = GetCubeSampledImageTypeId();

      ptr_cube_sampled_image_type = AllocateId();
      {
        u32 ops[] = {ptr_cube_sampled_image_type,
                     spv::StorageClassUniformConstant,
                     cube_sampled_image_type_id};
        EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
      }
    }
  }

  // Create texture variables for each used binding
  for (u8 binding = 0; binding < 32; binding++) {
    if (!(analysis.usedTextureMask & (1 << binding)))
      continue;

    // Select the correct pointer type based on texture type
    u32 ptr_type;
    if (textureIsArray[binding]) {
      ptr_type = ptr_array_sampled_image_type;
    } else if (textureIsCubemap[binding]) {
      ptr_type = ptr_cube_sampled_image_type;
    } else {
      ptr_type = ptr_sampled_image_type;
    }

    u32 tex_var_id = AllocateId();
    {
      u32 ops[] = {ptr_type, tex_var_id, spv::StorageClassUniformConstant};
      EmitToSection(&globals, spv::OpVariable, ops, 3);
    }

    u32 set_val[] = {0};
    u8 textureBinding =
        ResolveVertexPullingCollisionBinding(vertexPullingConfig, set_val[0],
                                            binding);
    u32 bind_val[] = {textureBinding};
    EmitDecoration(tex_var_id, spv::DecorationDescriptorSet, set_val, 1);
    EmitDecoration(tex_var_id, spv::DecorationBinding, bind_val, 1);

    textureIds[binding] = tex_var_id;
    bindingSets[resourceCount] = 0;
    bindingIndices[resourceCount] = textureBinding;
    resourceCount++;
  }

  // ============= Storage Buffers =============
  if (analysis.usedStorageBufferMask != 0) {
    // Create default runtime array type for storage buffers without struct
    // types
    u32 default_float4_type = GetTypeId(CoreType::FLOAT4);
    u32 default_runtime_array_type = AllocateId();
    {
      u32 ops[] = {default_runtime_array_type, default_float4_type};
      EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
    }
    {
      // Array stride decoration for default type
      u32 stride_ops[] = {default_runtime_array_type, spv::DecorationArrayStride,
                          16};
      EmitToSection(&decorations, spv::OpDecorate, stride_ops, 3);
    }

    // Create default struct containing runtime array
    u32 default_ssbo_struct_type = AllocateId();
    {
      u32 ops[] = {default_ssbo_struct_type, default_runtime_array_type};
      EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
    }

    // Block decoration for default type
    EmitDecoration(default_ssbo_struct_type, spv::DecorationBlock, nullptr, 0);

    {
      // Member offset for default type
      u32 member_offset[] = {default_ssbo_struct_type, 0, spv::DecorationOffset,
                            0};
      EmitToSection(&decorations, spv::OpMemberDecorate, member_offset, 4);
    }

    // Default pointer type
    u32 default_ptr_ssbo_type = AllocateId();
    {
      u32 ops[] = {default_ptr_ssbo_type, spv::StorageClassStorageBuffer,
                   default_ssbo_struct_type};
      EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
    }

    // Create variables for each used binding
    for (u8 binding = 0; binding < 32; binding++) {
      if (!(analysis.usedStorageBufferMask & (1 << binding)))
        continue;

      // Check if this storage buffer has a struct type
      // First check IR's discovered buffer element types (from usage context)
      u32 structTypeHash = ir->bufferElementStructTypes[binding];

      // Then check symbol table as fallback
      if (structTypeHash == 0 && symbols) {
        for (u32 r = 0; r < symbols->resources.count; r++) {
          const ResourceData &resData = symbols->resources[r];
          if (resData.bindingIndex == binding &&
              resData.type == ResourceBinding::Buffer &&
              resData.structTypeHash != 0) {
            structTypeHash = resData.structTypeHash;
            break;
          }
        }
      }

      u32 ptr_ssbo_type;
      u32 ssbo_struct_type;

      if (structTypeHash != 0) {
        // Use the actual struct type for this storage buffer
        u32 elem_struct_type = GetStructTypeId(structTypeHash);
        if (elem_struct_type != 0) {
          // For storage buffer arrays, we need:
          // 1. Element struct type (Particle)
          // 2. RuntimeArray of that struct
          // 3. Wrapper struct containing the RuntimeArray (Block-decorated)
          // 4. Pointer to wrapper struct

          // Create RuntimeArray type
          u32 runtime_array_type = AllocateId();
          {
            u32 ops[] = {runtime_array_type, elem_struct_type};
            EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
          }

          // ArrayStride decoration for the runtime array
          // Calculate struct size from IR struct info
          u32 structSize = 32; // Default fallback
          for (u32 i = 0; i < ir->structTypeCount; i++) {
            if (ir->structTypes[i].nameHash == structTypeHash) {
              structSize = ir->structTypes[i].totalSize;
              break;
            }
          }
          // Align to 16 bytes (std430 alignment for structs)
          structSize = (structSize + 15) & ~15;
          u32 stride_ops[] = {runtime_array_type, spv::DecorationArrayStride,
                              structSize};
          EmitToSection(&decorations, spv::OpDecorate, stride_ops, 3);

          // Create wrapper struct type containing the runtime array
          ssbo_struct_type = AllocateId();
          {
            u32 ops[] = {ssbo_struct_type, runtime_array_type};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
          }

          // Block decoration for wrapper struct
          EmitDecoration(ssbo_struct_type, spv::DecorationBlock, nullptr, 0);

          // Member offset for the runtime array member
          u32 member_offset[] = {ssbo_struct_type, 0, spv::DecorationOffset, 0};
          EmitToSection(&decorations, spv::OpMemberDecorate, member_offset, 4);

          // Pointer type for the wrapper struct
          ptr_ssbo_type = AllocateId();
          u32 ops[] = {ptr_ssbo_type, spv::StorageClassStorageBuffer,
                       ssbo_struct_type};
          EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
        } else {
          // Fallback to default
          ptr_ssbo_type = default_ptr_ssbo_type;
        }
      } else {
        // Check for primitive element type discovered from shader usage
        CoreType elementCoreType =
            static_cast<CoreType>(ir->bufferElementCoreTypes[binding]);
        if (elementCoreType != CoreType::VOID &&
            elementCoreType != CoreType::CUSTOM) {
          // Create runtime array with the discovered primitive element type
          u32 elem_type = GetTypeId(elementCoreType);
          u32 runtime_array_type = AllocateId();
          {
            u32 ops[] = {runtime_array_type, elem_type};
            EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
          }

          // ArrayStride decoration - calculate element size (std430 layout).
          // Vulkan's std140/std430/relaxed block layout require vec3 array
          // elements to be 16-byte aligned; emitting stride 12 produces SPIR-V
          // that fails spirv-val and is rejected by conformant drivers.
          u32 elemSize = 4; // default for float/int/uint
          switch (elementCoreType) {
          case CoreType::FLOAT2:
          case CoreType::INT2:
          case CoreType::UINT2:
            elemSize = 8;
            break;
          case CoreType::FLOAT3:
          case CoreType::INT3:
          case CoreType::UINT3:
            elemSize = 16;
            break;
          case CoreType::FLOAT4:
          case CoreType::INT4:
          case CoreType::UINT4:
            elemSize = 16;
            break;
          // Matrices: stored as arrays of column vectors, each aligned to 16
          // bytes
          case CoreType::MAT2:
            elemSize = 32;
            break; // 2 columns × 16 bytes
          case CoreType::MAT3:
            elemSize = 48;
            break; // 3 columns × 16 bytes
          case CoreType::MAT4:
            elemSize = 64;
            break; // 4 columns × 16 bytes
          default:
            elemSize = 4;
            break;
          }
          u32 stride_ops[] = {runtime_array_type, spv::DecorationArrayStride,
                              elemSize};
          EmitToSection(&decorations, spv::OpDecorate, stride_ops, 3);

          // Create wrapper struct containing the runtime array
          ssbo_struct_type = AllocateId();
          {
            u32 ops[] = {ssbo_struct_type, runtime_array_type};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
          }

          // Block decoration
          EmitDecoration(ssbo_struct_type, spv::DecorationBlock, nullptr, 0);

          // Member offset
          u32 member_offset[] = {ssbo_struct_type, 0, spv::DecorationOffset, 0};
          EmitToSection(&decorations, spv::OpMemberDecorate, member_offset, 4);

          // Matrix-specific decorations (required for mat2, mat3, mat4)
          if (elementCoreType == CoreType::MAT2 ||
              elementCoreType == CoreType::MAT3 ||
              elementCoreType == CoreType::MAT4) {
            u32 col_major[] = {ssbo_struct_type, 0, spv::DecorationColMajor};
            EmitToSection(&decorations, spv::OpMemberDecorate, col_major, 3);

            u32 matrix_stride[] = {ssbo_struct_type, 0,
                                   spv::DecorationMatrixStride, 16};
            EmitToSection(&decorations, spv::OpMemberDecorate, matrix_stride,
                          4);
          }

          // Pointer type
          ptr_ssbo_type = AllocateId();
          u32 ops[] = {ptr_ssbo_type, spv::StorageClassStorageBuffer,
                       ssbo_struct_type};
          EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
        } else {
          ptr_ssbo_type = default_ptr_ssbo_type;
        }
      }

      u32 ssbo_var_id = AllocateId();
      {
        u32 ops[] = {ptr_ssbo_type, ssbo_var_id,
                     spv::StorageClassStorageBuffer};
        EmitToSection(&globals, spv::OpVariable, ops, 3);
      }

      u32 set_val[] = {1};
      u8 actualBinding =
          ResolveVertexPullingCollisionBinding(vertexPullingConfig, set_val[0],
                                              binding);
      u32 bind_val[] = {actualBinding};
      EmitDecoration(ssbo_var_id, spv::DecorationDescriptorSet, set_val, 1);
      EmitDecoration(ssbo_var_id, spv::DecorationBinding, bind_val, 1);

      // Emit name for debugging - look up resource name from symbol table
      if (emitDebugNames && symbols) {
        // Find the symbol that corresponds to this resource binding
        for (u32 s = 0; s < symbols->symbols.count; s++) {
          const Symbol &sym = symbols->symbols[s];
          if (sym.kind == SymbolKind::RESOURCE &&
              sym.index < symbols->resources.count) {
            const ResourceData &resData = symbols->resources[sym.index];
            if (resData.bindingIndex == binding &&
                resData.type == ResourceBinding::Buffer) {
              std::string bufferName =
                  ReverseLookup::GetString(sym.name.nameHash);
              if (!bufferName.empty()) {
                EmitName(ssbo_var_id, bufferName.c_str());
              }
              break;
            }
          }
        }
      }

      storageBufferIds[binding] = ssbo_var_id;
      bindingSets[resourceCount] = 1;
      bindingIndices[resourceCount] = actualBinding;
      resourceCount++;
    }
  }

  // ============= Storage Images (read/write textures) =============
  if (analysis.usedStorageImageMask != 0) {
    // Get or create storage image type
    u32 storage_image_type = GetStorageImageTypeId();

    // Create pointer type for storage image (UniformConstant storage class)
    if (ptrStorageImageTypeId == 0) {
      ptrStorageImageTypeId = AllocateId();
      u32 ptr_ops[] = {ptrStorageImageTypeId, spv::StorageClassUniformConstant,
                       storage_image_type};
      EmitToSection(&typesConstants, spv::OpTypePointer, ptr_ops, 3);
    }

    // Create variables for each used storage image binding
    for (u8 binding = 0; binding < 32; binding++) {
      if (!(analysis.usedStorageImageMask & (1 << binding)))
        continue;

      u32 img_var_id = AllocateId();
      {
        u32 ops[] = {ptrStorageImageTypeId, img_var_id,
                     spv::StorageClassUniformConstant};
        EmitToSection(&globals, spv::OpVariable, ops, 3);
      }

      u32 set_val[] = {0};
      u8 actualBinding =
          ResolveVertexPullingCollisionBinding(vertexPullingConfig, set_val[0],
                                              binding);
      u32 bind_val[] = {actualBinding};
      EmitDecoration(img_var_id, spv::DecorationDescriptorSet, set_val, 1);
      EmitDecoration(img_var_id, spv::DecorationBinding, bind_val, 1);

      // NonReadable decoration for write-only images
      EmitDecoration(img_var_id, spv::DecorationNonReadable, nullptr, 0);

      storageImageIds[binding] = img_var_id;
      bindingSets[resourceCount] = 0;
      bindingIndices[resourceCount] = actualBinding;
      resourceCount++;
    }
  }
}

// ============= Final Assembly =============


} // namespace BWSL
