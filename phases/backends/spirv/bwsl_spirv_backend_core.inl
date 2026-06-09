// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Builder initialization, preamble, entry-point emission, and core setup.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

void SPIRVBuilder::Initialize(BWSL_Arena *arena, IR::IRProgram *ir,
                              ShaderStage stage, const SymbolTableData *symbols,
                              CFG *cfg) {
  this->arena = arena;
  this->ir = ir;
  this->stage = stage;
  this->symbols = symbols;
  this->cfg = cfg;
  workgroupSizeX = 1;
  workgroupSizeY = 1;
  workgroupSizeZ = 1;

  // Analyze IR to determine capabilities, resources, and I/O requirements
  AnalyzeIR(&analysis, ir);
  spvVersion =
      analysis.Has(IRAnalysis::CAP_WAVE_OPS) ? SpvVersion_1_3 : SpvVersion_1_2;

  // Initialize ID management
  nextId = 1;
  idCapacity = ir->registerCount + 256;
  spirvIds = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  idTypes = (u16 *)arena->Allocate(idCapacity * sizeof(u16), 64);
  idDecorations = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  hasPreAllocatedId = (bool *)arena->Allocate(idCapacity * sizeof(bool), 64);
  localVarIds = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  localArrayVarIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  localArrayElemPtrTypes = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  memset(spirvIds, 0, idCapacity * sizeof(u32));
  memset(idTypes, 0, idCapacity * sizeof(u16));
  memset(idDecorations, 0, idCapacity * sizeof(u32));
  memset(hasPreAllocatedId, 0, idCapacity * sizeof(bool));
  memset(localVarIds, 0, idCapacity * sizeof(u32));
  memset(localArrayVarIds, 0, 32 * sizeof(u32));
  memset(localArrayElemPtrTypes, 0, 32 * sizeof(u32));

  // Initialize storage pointer element type tracking
  storagePtrElemTypes = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  memset(storagePtrElemTypes, 0, idCapacity * sizeof(u32));

  // Initialize storage pointer storage class tracking
  storagePtrStorageClass = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  memset(storagePtrStorageClass, 0, idCapacity * sizeof(u32));

  // Initialize type arrays
  memset(typeIds, 0, sizeof(typeIds));
  compositeTypeCount = 0;
  compositeTypeIds = (u32 *)arena->Allocate(256 * sizeof(u32), 64);
  compositeTypeHashes = (u32 *)arena->Allocate(256 * sizeof(u32), 64);

  // Initialize struct type tracking
  structTypeCount = 0;
  structTypeIds = (u32 *)arena->Allocate(64 * sizeof(u32), 64);
  structTypeHashes = (u32 *)arena->Allocate(64 * sizeof(u32), 64);
  memset(structTypeIds, 0, 64 * sizeof(u32));
  memset(structTypeHashes, 0, 64 * sizeof(u32));

  // Initialize struct field type IDs (64 structs * 32 fields each)
  structFieldTypeIds =
      (u32 *)arena->Allocate(64 * MAX_FIELDS_PER_STRUCT * sizeof(u32), 64);
  memset(structFieldTypeIds, 0, 64 * MAX_FIELDS_PER_STRUCT * sizeof(u32));

  // Initialize struct array field tracking (tracks registers holding struct
  // field arrays)
  regIsStructArrayField =
      (bool *)arena->Allocate(idCapacity * sizeof(bool), 64);
  memset(regIsStructArrayField, 0, idCapacity * sizeof(bool));

  // Initialize SPIR-V type overrides (for when IR type differs from actual
  // SPIR-V type)
  spirvTypeOverrides = (u32 *)arena->Allocate(idCapacity * sizeof(u32), 64);
  memset(spirvTypeOverrides, 0, idCapacity * sizeof(u32));

  // Initialize constant pools
  constantCount = 0;
  floatConstantIds = (u32 *)arena->Allocate(ir->floatCount * sizeof(u32), 64);
  intConstantIds = (u32 *)arena->Allocate(ir->intCount * sizeof(u32), 64);
  uintConstantIds = (u32 *)arena->Allocate(ir->intCount * sizeof(u32), 64);
  constantHashes =
      (u32 *)arena->Allocate((ir->floatCount + ir->intCount) * sizeof(u32), 64);
  memset(floatConstantIds, 0, ir->floatCount * sizeof(u32));
  memset(intConstantIds, 0, ir->intCount * sizeof(u32));
  memset(uintConstantIds, 0, ir->intCount * sizeof(u32));

  // Initialize sections
  auto initSection = [arena](Section *s, u32 initial_capacity) {
    s->words = (u32 *)arena->Allocate(initial_capacity * sizeof(u32), 64);
    s->count = 0;
    s->capacity = initial_capacity;
  };

  initSection(&capabilities, 32);
  initSection(&extensions, 32);
  initSection(&extInstImports, 32);
  initSection(&memoryModel, 8);
  initSection(&entryPoints, 64);
  initSection(&executionModes, 32);
  initSection(&debugNames, 256);
  initSection(&decorations, 256);
  initSection(&typesConstants, 512);
  initSection(&globals, 128);
  initSection(&functions, 2048);

  // Initialize current function buffer
  currentFunctionCapacity = 512;
  currentFunction =
      (u32 *)arena->Allocate(currentFunctionCapacity * sizeof(u32), 64);
  currentFunctionSize = 0;

  // Initialize block management
  blockCount = 0;
  blockLabels = (u32 *)arena->Allocate(256 * sizeof(u32), 64);
  blockIRIndices = (u32 *)arena->Allocate(256 * sizeof(u32), 64);
  blockMergePoints = (u32 *)arena->Allocate(256 * sizeof(u32), 64);
  memset(blockLabels, 0, 256 * sizeof(u32));

  // Initialize interface variables
  inputCount = outputCount = 0;
  inputIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  inputLocations = (u8 *)arena->Allocate(32 * sizeof(u8), 64);
  outputIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  outputLocations = (u8 *)arena->Allocate(32 * sizeof(u8), 64);

  // Initialize resource bindings
  resourceCount = 0;
  uniformBufferIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  textureIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  samplerIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  storageBufferIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  storageImageIds = (u32 *)arena->Allocate(32 * sizeof(u32), 64);
  bindingSets = (u8 *)arena->Allocate(128 * sizeof(u8), 64);
  bindingIndices = (u8 *)arena->Allocate(128 * sizeof(u8), 64);

  // Allocate entry point and import extended instruction set
  entryPointId = AllocateId();
  glslStd450Id = AllocateId();

  // Emit the preamble immediately
  EmitPreamble();
}

// ============= Preamble Emission =============
void SPIRVBuilder::EmitPreamble() {
  // 1. Capabilities - emit based on IR analysis results

  // Base shader capability - required for all shaders
  EmitCapability(spv::CapabilityShader);

  // Derivative capabilities (based on IR analysis)
  if (analysis.HasAny(IRAnalysis::CAP_DERIVATIVES |
                      IRAnalysis::CAP_FINE_DERIVATIVES |
                      IRAnalysis::CAP_COARSE_DERIVATIVES)) {
    EmitCapability(spv::CapabilityDerivativeControl);
  }

  // Wave/subgroup operations (SPIR-V 1.3+)
  if (analysis.Has(IRAnalysis::CAP_WAVE_OPS)) {
    // GroupNonUniform = 61, GroupNonUniformArithmetic = 63,
    // GroupNonUniformBallot = 64, GroupNonUniformShuffle = 65
    EmitCapability(static_cast<spv::Capability>(61)); // GroupNonUniform
    EmitCapability(
        static_cast<spv::Capability>(63)); // GroupNonUniformArithmetic
    EmitCapability(static_cast<spv::Capability>(64)); // GroupNonUniformBallot
    EmitCapability(static_cast<spv::Capability>(62)); // GroupNonUniformVote
    EmitCapability(static_cast<spv::Capability>(65)); // GroupNonUniformShuffle
  }

  // Atomic operations: Vulkan does NOT require any capability for SSBO/workgroup
  // atomics beyond Shader. AtomicStorage is for OpenCL/Kernel contexts only and
  // is explicitly disallowed by spirv-val under vulkan1.x target environments.

  // Image read/write capabilities
  if (analysis.Has(IRAnalysis::CAP_IMAGE_STORE)) {
    EmitCapability(spv::CapabilityStorageImageWriteWithoutFormat);
  }
  if (analysis.Has(IRAnalysis::CAP_IMAGE_LOAD)) {
    EmitCapability(spv::CapabilityStorageImageReadWithoutFormat);
  }
  if (analysis.Has(IRAnalysis::CAP_IMAGE_QUERY)) {
    EmitCapability(spv::CapabilityImageQuery);
  }
  if (analysis.Has(IRAnalysis::CAP_IMAGE_GATHER_EXT)) {
    EmitCapability(spv::CapabilityImageGatherExtended);
  }

  // 64-bit types
  if (analysis.Has(IRAnalysis::CAP_INT64)) {
    EmitCapability(spv::CapabilityInt64);
  }
  if (analysis.Has(IRAnalysis::CAP_FLOAT64)) {
    EmitCapability(spv::CapabilityFloat64);
  }

  // Storage buffer capability (for buffer load/store)
  // Note: For SPIR-V 1.2+, storage buffers use StorageBuffer storage class
  // which requires SPV_KHR_storage_buffer_storage_class extension
  // We don't need StorageBuffer16BitAccess unless we actually use 16-bit types

  // Texture sampling capabilities
  if (analysis.Has(IRAnalysis::CAP_SAMPLED_1D)) {
    EmitCapability(spv::CapabilitySampled1D);
  }
  if (analysis.Has(IRAnalysis::CAP_IMAGE_1D)) {
    EmitCapability(spv::CapabilityImage1D);
  }
  if (analysis.Has(IRAnalysis::CAP_SAMPLED_CUBE)) {
    EmitCapability(spv::CapabilitySampledCubeArray);
  }

  // Clip/cull distance
  if (analysis.Has(IRAnalysis::CAP_CLIP_DISTANCE)) {
    EmitCapability(spv::CapabilityClipDistance);
  }
  if (analysis.Has(IRAnalysis::CAP_CULL_DISTANCE)) {
    EmitCapability(spv::CapabilityCullDistance);
  }

  // Storage buffer extension for vertex pulling modes or any storage buffer
  // usage SPIR-V 1.2 requires this extension for StorageBuffer storage class
  if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers ||
      vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets ||
      analysis.Has(IRAnalysis::CAP_STORAGE_BUFFER)) {
    EmitExtension("SPV_KHR_storage_buffer_storage_class");
  }

  // 2. Import GLSL.std.450 extended instruction set
  // OpExtInstImport format: word_count | op, result_id, "GLSL.std.450\0"
  const char *extName = "GLSL.std.450";
  u32 nameLen = (u32)(strlen(extName) + 1); // Include null terminator
  u32 nameWords = (nameLen + 3) / 4; // Round up to word boundary

  u32 wordCount = 2 + nameWords; // op + result + name words

  // Grow section if needed
  if (extInstImports.count + wordCount > extInstImports.capacity) {
    GrowSection(&extInstImports);
  }

  extInstImports.words[extInstImports.count++] =
      (wordCount << 16) | spv::OpExtInstImport;
  extInstImports.words[extInstImports.count++] = glslStd450Id;

  // Copy name as words (with padding)
  u32 *namePtr = &extInstImports.words[extInstImports.count];
  memset(namePtr, 0, nameWords * 4); // Zero-fill for padding
  memcpy(namePtr, extName, nameLen);
  extInstImports.count += nameWords;

  // 3. Memory Model
  // OpMemoryModel Logical GLSL450
  u32 memModelOps[] = {spv::AddressingModelLogical, spv::MemoryModelGLSL450};
  EmitToSection(&memoryModel, spv::OpMemoryModel, memModelOps, 2);
}

void SPIRVBuilder::EmitEntryPoint() {
  // Determine execution model based on shader stage
  spv::ExecutionModel execModel;
  const char *entryName = "main";

  switch (stage) {
  case ShaderStage::Vertex:
    execModel = spv::ExecutionModelVertex;
    break;
  case ShaderStage::Fragment:
    execModel = spv::ExecutionModelFragment;
    break;
  case ShaderStage::Compute:
    execModel = spv::ExecutionModelGLCompute;
    break;
  default:
    execModel = spv::ExecutionModelVertex;
    break;
  }

  // OpEntryPoint format: exec_model, entry_point_id, "name", interface_vars...
  u32 nameLen = (u32)(strlen(entryName) + 1);
  u32 nameWords = (nameLen + 3) / 4;

  // Count interface variables (inputs + outputs)
  u32 interfaceCount = inputCount + outputCount;
  u32 wordCount =
      3 + nameWords +
      interfaceCount; // op + exec_model + entry_id + name + interfaces

  if (entryPoints.count + wordCount > entryPoints.capacity) {
    GrowSection(&entryPoints);
  }

  entryPoints.words[entryPoints.count++] =
      (wordCount << 16) | spv::OpEntryPoint;
  entryPoints.words[entryPoints.count++] = execModel;
  entryPoints.words[entryPoints.count++] = entryPointId;

  // Copy name
  u32 *namePtr = &entryPoints.words[entryPoints.count];
  memset(namePtr, 0, nameWords * 4);
  memcpy(namePtr, entryName, nameLen);
  entryPoints.count += nameWords;

  // Add interface variable IDs
  for (u32 i = 0; i < inputCount; i++) {
    entryPoints.words[entryPoints.count++] = inputIds[i];
  }
  for (u32 i = 0; i < outputCount; i++) {
    entryPoints.words[entryPoints.count++] = outputIds[i];
  }

  // Emit execution modes
  switch (stage) {
  case ShaderStage::Fragment:
    // OriginUpperLeft - standard for Vulkan
    {
      u32 execModeOps[] = {entryPointId, spv::ExecutionModeOriginUpperLeft};
      EmitToSection(&executionModes, spv::OpExecutionMode, execModeOps, 2);
    }
    if (analysis.usedOutputMask & (1 << OutputSlot::DEPTH)) {
      u32 execModeOps[] = {entryPointId, spv::ExecutionModeDepthReplacing};
      EmitToSection(&executionModes, spv::OpExecutionMode, execModeOps, 2);
    }
    break;
  case ShaderStage::Compute:
    // LocalSize - workgroup size (default 1,1,1 - should be configurable)
    {
      u32 execModeOps[] = {entryPointId, spv::ExecutionModeLocalSize,
                           workgroupSizeX, workgroupSizeY, workgroupSizeZ};
      EmitToSection(&executionModes, spv::OpExecutionMode, execModeOps, 5);
    }
    break;
  default:
    break;
  }
}

// ============= Type Management =============


} // namespace BWSL
