// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Interface variables, stage inputs/outputs, builtins, shared variables, and vertex pulling declarations.

u32 SPIRVBuilder::CreateInterfaceVariable(CoreType type,
                                          spv::StorageClass storage,
                                          u32 location, spv::BuiltIn builtin) {
  u32 type_id = GetTypeId(type);
  u32 ptr_type_id = GetPointerTypeId(type_id, storage);
  u32 var_id = AllocateId();

  // OpVariable
  u32 ops[] = {ptr_type_id, var_id, static_cast<u32>(storage)};
  EmitToSection(&globals, spv::OpVariable, ops, 3);

  // Apply decoration
  if (builtin != spv::BuiltInMax) {
    // BuiltIn decoration
    u32 builtin_val[] = {static_cast<u32>(builtin)};
    EmitDecoration(var_id, spv::DecorationBuiltIn, builtin_val, 1);
  } else {
    // Location decoration
    u32 loc[] = {location};
    EmitDecoration(var_id, spv::DecorationLocation, loc, 1);
  }

  return var_id;
}

// Helper: Get fallback attribute type by index
// With user-defined attributes, only position (index 0) is known to be float3
static CoreType GetFallbackAttributeType(u32 attrIdx) {
  // Index 0 is always position (float3), everything else defaults to float4
  return (attrIdx == 0) ? CoreType::FLOAT3 : CoreType::FLOAT4;
}

// Helper: Get fallback output type by slot
static CoreType GetFallbackOutputType(u32 slot) {
  switch (slot) {
  case OutputSlot::POSITION:
    return CoreType::FLOAT4;
  case OutputSlot::COLOR:
    return CoreType::FLOAT4;
  case OutputSlot::DEPTH:
    return CoreType::FLOAT;
  default:
    return CoreType::FLOAT4; // varyings
  }
}

void SPIRVBuilder::DeclareInputOutput() {
  // ============= Vertex Shader =============
  if (stage == ShaderStage::Vertex) {
    // Declare built-in inputs (vertex_id, instance_id) if used by shader code
    // These are separate from vertex pulling which also uses vertex_id
    // internally
    if (analysis.UsesVertexId() && vertexIdVarId == 0) {
      DeclareVertexIdBuiltin();
    }
    if (analysis.UsesInstanceId() && instanceIdVarId == 0) {
      DeclareInstanceIdBuiltin();
    }

    // Check vertex pulling mode
    if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers ||
        vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets) {
      // Use vertex pulling - attributes come from storage buffers
      // DeclareVertexIdBuiltin may have already been called above
      if (vertexIdVarId == 0) {
        DeclareVertexIdBuiltin();
      }
      DeclareVertexPullingBuffers();
    } else {
      // Traditional interleaved mode - attributes via Input variables with
      // Location
      // --- Inputs: Only declare attributes that are actually used ---
      for (u32 attrIdx = 0; attrIdx < 16; attrIdx++) {
        if (!(analysis.usedAttributeMask & (1 << attrIdx)))
          continue;

        // Get type from analysis (captured from IR), fall back to symbol table,
        // then defaults
        CoreType type = static_cast<CoreType>(analysis.attributeTypes[attrIdx]);
        if (type == CoreType::VOID || type == CoreType::INVALID) {
          // Try symbol table
          if (symbols) {
            for (u32 i = 0; i < symbols->attributes.count; i++) {
              if (symbols->attributes[i].attributeIndex == attrIdx) {
                type = symbols->attributes[i].typeInfo.coreType;
                break;
              }
            }
          }
          // Final fallback
          if (type == CoreType::VOID || type == CoreType::INVALID) {
            type = GetFallbackAttributeType(attrIdx);
          }
        }

        u32 var_id = CreateInterfaceVariable(type, spv::StorageClassInput,
                                             attrIdx, spv::BuiltInMax);
        inputIds[inputCount] = var_id;
        inputLocations[inputCount] = attrIdx;
        inputCount++;
      }
    }

    // --- Outputs: Position builtin (if used) ---
    if (analysis.usedOutputMask & (1 << OutputSlot::POSITION)) {
      u32 var_id = CreateInterfaceVariable(
          CoreType::FLOAT4, spv::StorageClassOutput, 0, spv::BuiltInPosition);
      outputIds[outputCount] = var_id;
      outputLocations[outputCount] = 0xFF; // Mark as builtin
      outputCount++;
    }

    // --- Outputs: User varyings (if used) ---
    // Support up to 16 varyings (VARYING0 through VARYING0+15) to match
    // fragment shader
    u32 locationCounter = 0;
    for (u32 slot = OutputSlot::VARYING0; slot <= OutputSlot::VARYING0 + 15;
         slot++) {
      if (!(analysis.usedOutputMask & (1 << slot)))
        continue;

      // Get type from analysis or use fallback
      CoreType type = static_cast<CoreType>(analysis.outputTypes[slot]);
      if (type == CoreType::VOID || type == CoreType::INVALID) {
        type = GetFallbackOutputType(slot);
      }

      u32 var_id = CreateInterfaceVariable(type, spv::StorageClassOutput,
                                           locationCounter, spv::BuiltInMax);

      // Emit consistent varying names for WebGL compatibility
      // (GLSL ES 300 matches varyings by name, not location)
      char varyingName[16];
      snprintf(varyingName, sizeof(varyingName), "varying%u", locationCounter);
      EmitName(var_id, varyingName);

      locationCounter++;
      outputIds[outputCount] = var_id;
      // Store the slot ID for OP_STORE_OUTPUT lookup
      outputLocations[outputCount] = slot;
      outputCount++;
    }
  }

  // ============= Fragment Shader =============
  else if (stage == ShaderStage::Fragment) {
    // Declare built-in inputs (FragCoord) if used by shader code
    if (analysis.UsesFragCoord() && fragCoordVarId == 0) {
      DeclareFragCoordBuiltin();
    }

    // --- Inputs: Varyings from vertex shader ---
    // Use usedInputMask which tracks OP_LOAD_INPUT usage (input.normal, etc.)
    // Slots are VARYING0+index, so location = slot - VARYING0 to match vertex
    // output IMPORTANT: Location must match vertex shader even if some varyings
    // are unused
    for (u32 slot = OutputSlot::VARYING0; slot <= OutputSlot::VARYING0 + 15;
         slot++) {
      if (!(analysis.usedInputMask & (1 << slot)))
        continue;

      // Get type from analysis or use fallback
      CoreType type = static_cast<CoreType>(analysis.inputTypes[slot]);
      if (type == CoreType::VOID || type == CoreType::INVALID) {
        type = CoreType::FLOAT3; // Default for varyings like normal
      }

      // Location = slot - VARYING0 to match vertex shader output locations
      // e.g., slot=VARYING0+1 → location=1, matching vertex "varying1"
      u32 location = slot - OutputSlot::VARYING0;
      u32 var_id = CreateInterfaceVariable(type, spv::StorageClassInput,
                                           location, spv::BuiltInMax);

      // Emit consistent varying names for WebGL compatibility
      // (GLSL ES 300 matches varyings by name, not location)
      char varyingName[16];
      snprintf(varyingName, sizeof(varyingName), "varying%u", location);
      EmitName(var_id, varyingName);

      inputIds[inputCount] = var_id;
      inputLocations[inputCount] =
          slot; // Store actual slot for OP_LOAD_INPUT lookup
      inputCount++;
    }

    // --- Output: Fragment color (if used) ---
    if (analysis.usedOutputMask & (1 << OutputSlot::COLOR)) {
      CoreType type =
          static_cast<CoreType>(analysis.outputTypes[OutputSlot::COLOR]);
      if (type == CoreType::VOID || type == CoreType::INVALID) {
        type = CoreType::FLOAT4;
      }
      u32 var_id = CreateInterfaceVariable(type, spv::StorageClassOutput, 0,
                                           spv::BuiltInMax);
      outputIds[outputCount] = var_id;
      // Store the slot ID (COLOR=1) for OP_STORE_OUTPUT lookup, not SPIR-V
      // location
      outputLocations[outputCount] = OutputSlot::COLOR;
      outputCount++;
    }

    // --- Output: Fragment depth (if used) ---
    if (analysis.usedOutputMask & (1 << OutputSlot::DEPTH)) {
      u32 var_id = CreateInterfaceVariable(
          CoreType::FLOAT, spv::StorageClassOutput, 0, spv::BuiltInFragDepth);
      outputIds[outputCount] = var_id;
      // Store the slot ID (DEPTH=16) for OP_STORE_OUTPUT lookup
      outputLocations[outputCount] = OutputSlot::DEPTH;
      outputLocations[outputCount] = 0xFF; // Mark as builtin
      outputCount++;
    }
  }

  // ============= Compute Shader =============
  else if (stage == ShaderStage::Compute) {
    // Compute shaders need builtin inputs for dispatch coordinates
    // Declare all compute built-ins and store their variable IDs for later use

    // GlobalInvocationId (uint3) - most commonly used
    if (analysis.UsesGlobalId()) {
      globalInvocationIdVarId =
          CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput, 0,
                                  spv::BuiltInGlobalInvocationId);
      inputIds[inputCount] = globalInvocationIdVarId;
      inputLocations[inputCount] = 0xFF; // Mark as builtin
      inputCount++;
    }

    // LocalInvocationId (uint3)
    if (analysis.UsesLocalId()) {
      localInvocationIdVarId =
          CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput, 0,
                                  spv::BuiltInLocalInvocationId);
      inputIds[inputCount] = localInvocationIdVarId;
      inputLocations[inputCount] = 0xFF;
      inputCount++;
    }

    // WorkgroupId (uint3)
    if (analysis.UsesWorkgroupId()) {
      workgroupIdVarId = CreateInterfaceVariable(
          CoreType::UINT3, spv::StorageClassInput, 0, spv::BuiltInWorkgroupId);
      inputIds[inputCount] = workgroupIdVarId;
      inputLocations[inputCount] = 0xFF;
      inputCount++;
    }

    // NumWorkgroups (uint3)
    if (analysis.UsesNumWorkgroups()) {
      numWorkgroupsVarId =
          CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput, 0,
                                  spv::BuiltInNumWorkgroups);
      inputIds[inputCount] = numWorkgroupsVarId;
      inputLocations[inputCount] = 0xFF;
      inputCount++;
    }

    // LocalInvocationIndex (uint) - single scalar, not uint3
    if (analysis.UsesLocalIndex()) {
      localInvocationIndexVarId =
          CreateInterfaceVariable(CoreType::UINT, spv::StorageClassInput, 0,
                                  spv::BuiltInLocalInvocationIndex);
      inputIds[inputCount] = localInvocationIndexVarId;
      inputLocations[inputCount] = 0xFF;
      inputCount++;
    }
  }
}

void SPIRVBuilder::DeclareSharedVariables() {
  if (!ir || ir->sharedVarCount == 0)
    return;

  for (u32 i = 0; i < ir->sharedVarCount; i++) {
    CoreType elemType = static_cast<CoreType>(ir->sharedTypes[i]);
    u32 elemTypeId = GetTypeId(elemType);
    if (elemTypeId == 0) {
      continue;
    }

    u32 varTypeId = elemTypeId;
    u32 arraySize = ir->sharedArraySizes[i];
    if (arraySize > 0) {
      u32 arrayTypeId = AllocateId();
      u32 lengthConstId = GetIntConstantId(arraySize, true);
      u32 arrayOps[] = {arrayTypeId, elemTypeId, lengthConstId};
      EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);
      varTypeId = arrayTypeId;
    }

    u32 ptrTypeId = GetPointerTypeId(varTypeId, spv::StorageClassWorkgroup);
    u32 varId = AllocateId();
    u32 varOps[] = {ptrTypeId, varId,
                    static_cast<u32>(spv::StorageClassWorkgroup)};
    EmitToSection(&globals, spv::OpVariable, varOps, 3);

    if (emitDebugNames) {
      std::string name = ReverseLookup::GetString(ir->sharedNameHashes[i]);
      if (!name.empty()) {
        EmitName(varId, name.c_str());
      }
    }

    u16 reg = ir->sharedRegisters[i];
    if (reg < idCapacity) {
      spirvIds[reg] = varId;
    }
  }
}

// ============= Vertex Pulling Implementation =============

void SPIRVBuilder::DeclareVertexIdBuiltin() {
  // Declare gl_VertexIndex (SPIR-V BuiltIn VertexIndex) for indexing into
  // attribute buffers
  u32 uint_type = GetTypeId(CoreType::UINT);
  u32 ptr_type = GetPointerTypeId(uint_type, spv::StorageClassInput);

  vertexIdVarId = AllocateId();

  // OpVariable for vertex ID input
  u32 ops[] = {ptr_type, vertexIdVarId,
               static_cast<u32>(spv::StorageClassInput)};
  EmitToSection(&globals, spv::OpVariable, ops, 3);

  // Decorate as BuiltIn VertexIndex
  u32 builtin_val[] = {static_cast<u32>(spv::BuiltInVertexIndex)};
  EmitDecoration(vertexIdVarId, spv::DecorationBuiltIn, builtin_val, 1);

  // Add to interface list for entry point
  inputIds[inputCount] = vertexIdVarId;
  inputLocations[inputCount] = 0xFF; // Mark as builtin
  inputCount++;
}

void SPIRVBuilder::DeclareInstanceIdBuiltin() {
  // Declare gl_InstanceIndex (SPIR-V BuiltIn InstanceIndex) for instanced
  // rendering
  u32 uint_type = GetTypeId(CoreType::UINT);
  u32 ptr_type = GetPointerTypeId(uint_type, spv::StorageClassInput);

  instanceIdVarId = AllocateId();

  // OpVariable for instance ID input
  u32 ops[] = {ptr_type, instanceIdVarId,
               static_cast<u32>(spv::StorageClassInput)};
  EmitToSection(&globals, spv::OpVariable, ops, 3);

  // Decorate as BuiltIn InstanceIndex
  u32 builtin_val[] = {static_cast<u32>(spv::BuiltInInstanceIndex)};
  EmitDecoration(instanceIdVarId, spv::DecorationBuiltIn, builtin_val, 1);

  // Add to interface list for entry point
  inputIds[inputCount] = instanceIdVarId;
  inputLocations[inputCount] = 0xFF; // Mark as builtin
  inputCount++;
}

void SPIRVBuilder::DeclareFragCoordBuiltin() {
  // Declare gl_FragCoord (SPIR-V BuiltIn FragCoord) for fragment position
  u32 float4_type = GetTypeId(CoreType::FLOAT4);
  u32 ptr_type = GetPointerTypeId(float4_type, spv::StorageClassInput);

  fragCoordVarId = AllocateId();

  // OpVariable for frag coord input
  u32 ops[] = {ptr_type, fragCoordVarId,
               static_cast<u32>(spv::StorageClassInput)};
  EmitToSection(&globals, spv::OpVariable, ops, 3);

  // Decorate as BuiltIn FragCoord
  u32 builtin_val[] = {static_cast<u32>(spv::BuiltInFragCoord)};
  EmitDecoration(fragCoordVarId, spv::DecorationBuiltIn, builtin_val, 1);

  // Add to interface list for entry point
  inputIds[inputCount] = fragCoordVarId;
  inputLocations[inputCount] = 0xFF; // Mark as builtin
  inputCount++;
}

void SPIRVBuilder::DeclareVertexPullingBuffers() {
  if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers) {
    // Declare one storage buffer per attribute
    u32 binding = vertexPullingConfig.baseBufferBinding;
    const u32 activeAttributeMask =
        vertexPullingConfig.attributeMask != 0 ? vertexPullingConfig.attributeMask
                                               : analysis.usedAttributeMask;

    for (u32 attrIdx = 0; attrIdx < 16; attrIdx++) {
      if (!(analysis.usedAttributeMask & (1 << attrIdx)))
        continue;
      if (!(activeAttributeMask & (1 << attrIdx)))
        continue;

      // Get type from analysis or fallback
      CoreType type = static_cast<CoreType>(analysis.attributeTypes[attrIdx]);
      if (type == CoreType::VOID || type == CoreType::INVALID) {
        type = GetFallbackAttributeType(attrIdx);
      }

      u32 buffer_id = CreateStorageBufferForAttribute(attrIdx, type, binding);
      attributeBufferIds[attrIdx] = buffer_id;
      binding++;
    }
  } else if (vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets) {
    // Declare unified vertex buffer (as byte array)
    // and offset table buffer

    // Unified buffer at base binding
    u32 uint_type = GetTypeId(CoreType::UINT);
    u32 runtime_array_type = AllocateId();
    {
      u32 ops[] = {runtime_array_type, uint_type};
      EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
    }

    // Decorate array stride
    u32 stride[] = {4}; // u32 stride
    EmitDecoration(runtime_array_type, spv::DecorationArrayStride, stride, 1);

    // Struct wrapper for buffer block
    u32 struct_type = AllocateId();
    {
      u32 ops[] = {struct_type, runtime_array_type};
      EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
    }
    EmitDecoration(struct_type, spv::DecorationBlock, nullptr, 0);

    // Member 0 offset decoration (OpMemberDecorate)
    EmitMemberDecoration(struct_type, 0, spv::DecorationOffset, 0);

    // Pointer type
    u32 ptr_type =
        GetPointerTypeId(struct_type, spv::StorageClassStorageBuffer);

    // Variable
    u32 buffer_id = AllocateId();
    {
      u32 ops[] = {ptr_type, buffer_id,
                   static_cast<u32>(spv::StorageClassStorageBuffer)};
      EmitToSection(&globals, spv::OpVariable, ops, 3);
    }

    // Bindings
    u32 set[] = {vertexPullingConfig.descriptorSet};
    EmitDecoration(buffer_id, spv::DecorationDescriptorSet, set, 1);
    u32 binding[] = {vertexPullingConfig.baseBufferBinding};
    EmitDecoration(buffer_id, spv::DecorationBinding, binding, 1);

    // Store in first slot (unified buffer)
    attributeBufferIds[0] = buffer_id;

    // Offset table at base binding + 1
    // Similar structure but for offset table
    u32 offset_struct_type = AllocateId();
    {
      u32 ops[] = {offset_struct_type, runtime_array_type};
      EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
    }
    EmitDecoration(offset_struct_type, spv::DecorationBlock, nullptr, 0);
    EmitMemberDecoration(offset_struct_type, 0, spv::DecorationOffset, 0);

    u32 offset_ptr_type =
        GetPointerTypeId(offset_struct_type, spv::StorageClassStorageBuffer);

    u32 offset_buffer_id = AllocateId();
    {
      u32 ops[] = {offset_ptr_type, offset_buffer_id,
                   static_cast<u32>(spv::StorageClassStorageBuffer)};
      EmitToSection(&globals, spv::OpVariable, ops, 3);
    }

    EmitDecoration(offset_buffer_id, spv::DecorationDescriptorSet, set, 1);
    u32 offset_binding[] = {vertexPullingConfig.baseBufferBinding + 1};
    EmitDecoration(offset_buffer_id, spv::DecorationBinding, offset_binding, 1);

    // Store offset table ID
    attributeBufferIds[1] = offset_buffer_id;
  }
}

u32 SPIRVBuilder::CreateStorageBufferForAttribute(u32 attrIdx,
                                                  CoreType elementType,
                                                  u32 binding) {
  // Create a storage buffer containing a runtime array of the attribute type
  // This maps to ModelData::attributeStreams[attrIdx]

  u32 element_type_id = GetTypeId(elementType);

  // Runtime array of the element type
  u32 runtime_array_type = AllocateId();
  {
    u32 ops[] = {runtime_array_type, element_type_id};
    EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
  }

  // Decorate array stride based on element type
  // Note: GLSL std430 requires vec3/ivec3/uvec3 to have 16-byte stride (same as
  // vec4) For Metal/HLSL we can use packed 12-byte stride
  u32 stride = 0;
  u32 vec3Stride = useStd430Padding ? 16 : 12;
  switch (elementType) {
  case CoreType::FLOAT:
    stride = 4;
    break;
  case CoreType::FLOAT2:
    stride = 8;
    break;
  case CoreType::FLOAT3:
    stride = vec3Stride;
    break;
  case CoreType::FLOAT4:
    stride = 16;
    break;
  case CoreType::INT:
    stride = 4;
    break;
  case CoreType::INT2:
    stride = 8;
    break;
  case CoreType::INT3:
    stride = vec3Stride;
    break;
  case CoreType::INT4:
    stride = 16;
    break;
  case CoreType::UINT:
    stride = 4;
    break;
  case CoreType::UINT2:
    stride = 8;
    break;
  case CoreType::UINT3:
    stride = vec3Stride;
    break;
  case CoreType::UINT4:
    stride = 16;
    break;
  // Matrices: stored as arrays of column vectors, each aligned to 16 bytes
  case CoreType::MAT2:
    stride = 32;
    break; // 2 columns × 16 bytes
  case CoreType::MAT3:
    stride = 48;
    break; // 3 columns × 16 bytes
  case CoreType::MAT4:
    stride = 64;
    break; // 4 columns × 16 bytes
  default:
    stride = 16;
    break;
  }

  u32 stride_val[] = {stride};
  EmitDecoration(runtime_array_type, spv::DecorationArrayStride, stride_val, 1);

  // Struct wrapper (required for storage buffers)
  u32 struct_type_id = AllocateId();
  {
    u32 ops[] = {struct_type_id, runtime_array_type};
    EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
  }

  // Decorate struct as Block (required for storage buffers)
  EmitDecoration(struct_type_id, spv::DecorationBlock, nullptr, 0);

  // Member 0 offset decoration (OpMemberDecorate requires: target, member,
  // decoration, [operands])
  EmitMemberDecoration(struct_type_id, 0, spv::DecorationOffset, 0);

  // Pointer to struct
  u32 ptr_type_id =
      GetPointerTypeId(struct_type_id, spv::StorageClassStorageBuffer);
  attributeBufferPtrTypeIds[attrIdx] = ptr_type_id;

  // Variable declaration
  u32 var_id = AllocateId();
  {
    u32 ops[] = {ptr_type_id, var_id,
                 static_cast<u32>(spv::StorageClassStorageBuffer)};
    EmitToSection(&globals, spv::OpVariable, ops, 3);
  }

  // Descriptor set decoration
  u32 set_val[] = {vertexPullingConfig.descriptorSet};
  EmitDecoration(var_id, spv::DecorationDescriptorSet, set_val, 1);

  // Binding decoration
  u32 binding_val[] = {binding};
  EmitDecoration(var_id, spv::DecorationBinding, binding_val, 1);

  // Name decoration for debugging
  // Emit debug names for attribute buffers if enabled
  if (emitDebugNames) {
    char attrName[32];
    snprintf(attrName, sizeof(attrName), "attribute_%u_buffer", attrIdx);
    EmitName(var_id, attrName);
  }

  return var_id;
}

// Helper: Check if type is a matrix
